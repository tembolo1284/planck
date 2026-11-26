#ifndef PLANCK_BOOK_ORDER_BOOK_HPP
#define PLANCK_BOOK_ORDER_BOOK_HPP

#include <algorithm>
#include <cstdint>

#include "planck/book/ladder_bitmap.hpp"
#include "planck/book/order.hpp"
#include "planck/book/order_store.hpp"
#include "planck/book/types.hpp"
#include "planck/core/contract.hpp"
#include "planck/core/hint.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/mem/arena.hpp"
#include "planck/mem/pool.hpp"

namespace planck::book {

struct BookConfig {
    core::SymbolId symbol{};
    // Lowest price the book accepts.
    core::Price band_low{};
    // Number of ticks in the band. Power of two in
    // [OrderBook::kMinBandTicks, OrderBook::kMaxBandTicks]. The book accepts
    // prices in [band_low, band_low + band_ticks).
    std::uint32_t band_ticks = 0;
};

// Price-time priority limit order book for one symbol.
//
// The price ladder is a flat array with one Level per tick in the band, so
// finding a price is one subtraction and one array index. Bids and asks share
// the array: in an uncrossed book every bid is below every ask, so a tick
// never holds both. Two bitmaps record which ticks are occupied on each side,
// and the best bid and best ask are cached.
//
// Costs: add without a match, and cancel, are O(1). A match is O(orders
// filled). Stepping to the next occupied level is O(1) via the bitmaps.
//
// Not thread safe. A book belongs to one matching thread.
class OrderBook {
public:
    static constexpr std::uint32_t kMinBandTicks = 64;
    static constexpr std::uint32_t kMaxBandTicks = LadderBitmap::kMaxBits;

    OrderBook() noexcept = default;
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Carve the ladder out of arena and attach to store, which must outlive
    // the book. Writes the whole ladder, so call from the owning thread.
    [[nodiscard]] core::Status init(const BookConfig& config, OrderStore& store,
                                    mem::Arena& arena) noexcept;

    // Enter an order. Fills are reported to sink in execution order before
    // this returns. See AddResult for how to read the outcome.
    template <TradeSink S>
    [[nodiscard]] AddResult add(const NewOrder& order, S& sink) noexcept;

    // Remove a resting order. UnknownOrder if id is not resting in this book.
    [[nodiscard]] CancelResult cancel(core::OrderId id) noexcept;

    // Best price and its total quantity on one side. valid is false if that
    // side is empty.
    [[nodiscard]] Quote best(core::Side side) const noexcept {
        PLANCK_EXPECT(initialised());
        const Offset offset = (side == core::Side::Buy) ? best_bid_ : best_ask_;
        if (offset == kNoLevel) {
            return Quote{};
        }
        return Quote{.price = price_at(offset), .qty = levels_[offset].total, .valid = true};
    }

    // Total quantity resting at a price. Zero if empty or outside the band.
    [[nodiscard]] std::uint64_t quantity_at(core::Price price) const noexcept {
        PLANCK_EXPECT(initialised());
        Offset offset = 0;
        return try_offset(price, offset) ? levels_[offset].total : 0;
    }

    [[nodiscard]] bool in_band(core::Price price) const noexcept {
        Offset offset = 0;
        return try_offset(price, offset);
    }

    [[nodiscard]] bool initialised() const noexcept { return levels_ != nullptr; }
    [[nodiscard]] core::SymbolId symbol() const noexcept { return symbol_; }
    [[nodiscard]] core::Price band_low() const noexcept { return band_low_; }
    [[nodiscard]] std::uint32_t band_ticks() const noexcept { return band_ticks_; }

private:
    // Distance in ticks from band_low_. Indexes levels_ and the bitmaps.
    using Offset = std::uint32_t;
    static constexpr Offset kNoLevel = LadderBitmap::kNone;

    [[nodiscard]] static constexpr AddResult rejected(RejectReason reason) noexcept {
        return AddResult{.reject = reason};
    }

    // True if an incoming order with this limit can trade against level.
    [[nodiscard]] static constexpr bool crosses(bool buy, Offset limit, Offset level) noexcept {
        return buy ? (level <= limit) : (level >= limit);
    }

    [[nodiscard]] bool try_offset(core::Price price, Offset& out) const noexcept {
        // 64-bit so the subtraction cannot overflow for any pair of prices.
        const std::int64_t distance =
            static_cast<std::int64_t>(price.raw()) - static_cast<std::int64_t>(band_low_.raw());
        if (distance < 0 || distance >= static_cast<std::int64_t>(band_ticks_)) {
            return false;
        }
        out = static_cast<Offset>(distance);
        return true;
    }

    [[nodiscard]] core::Price price_at(Offset offset) const noexcept {
        PLANCK_EXPECT(offset < band_ticks_);
        return core::Price{static_cast<std::int32_t>(band_low_.raw() + static_cast<std::int32_t>(offset))};
    }

    // Fill-or-kill pre-check: is there enough quantity at crossing prices?
    [[nodiscard]] bool can_fill(core::Side side, Offset limit, core::Qty want) const noexcept;

    template <TradeSink S>
    [[nodiscard]] core::Qty match(core::Side side, Offset limit, core::OrderId aggressor,
                                  core::Qty want, S& sink) noexcept;

    // Remove the head order of a level after it has been fully filled.
    void pop_filled_head(bool aggressor_is_buy, Offset offset, Level& level, mem::Index slot,
                         const Order& filled) noexcept;

    // Append an order to the back of its price level.
    void rest(mem::Index slot, const NewOrder& order, Offset offset, core::Qty qty) noexcept;

    // A level just emptied. Clear its bit and, if it was the best, find the
    // next best.
    void level_emptied(bool level_is_bid, Offset offset) noexcept {
        if (level_is_bid) {
            bids_.clear(offset);
            if (offset == best_bid_) {
                best_bid_ = bids_.highest_upto(offset);
            }
        } else {
            asks_.clear(offset);
            if (offset == best_ask_) {
                best_ask_ = asks_.lowest_from(offset);
            }
        }
    }

    Level* levels_ = nullptr;
    OrderStore* store_ = nullptr;
    Offset best_bid_ = kNoLevel;
    Offset best_ask_ = kNoLevel;
    std::uint32_t band_ticks_ = 0;
    core::Price band_low_{};
    core::SymbolId symbol_{};
    LadderBitmap bids_;
    LadderBitmap asks_;
};

template <TradeSink S>
AddResult OrderBook::add(const NewOrder& order, S& sink) noexcept {
    PLANCK_EXPECT(initialised());
    PLANCK_EXPECT(core::is_valid(order.side) && core::is_valid(order.type) &&
                  core::is_valid(order.tif));

    if (order.qty == core::Qty{0}) [[unlikely]] {
        return rejected(RejectReason::InvalidQuantity);
    }
    const bool buy = (order.side == core::Side::Buy);
    const bool market = (order.type == core::OrderType::Market);

    // A market order is a limit order at the far edge of the band.
    Offset limit = buy ? (band_ticks_ - 1) : 0;
    if (!market && !try_offset(order.price, limit)) [[unlikely]] {
        return rejected(RejectReason::PriceOutOfBand);
    }
    if (order.tif == core::TimeInForce::FillOrKill && !can_fill(order.side, limit, order.qty)) {
        return rejected(RejectReason::NotFillable);
    }

    // Reserve the slot before matching. Once fills have been reported there
    // is no way to back out, so "no room to rest" must be decided up front.
    const bool may_rest = !market && (order.tif == core::TimeInForce::Day);
    mem::Index slot = mem::kNullIndex;
    core::OrderId id = core::kNoOrderId;
    if (may_rest) {
        slot = store_->acquire();
        if (slot == mem::kNullIndex) [[unlikely]] {
            return rejected(RejectReason::BookFull);
        }
        id = (*store_)[slot].id;
    } else {
        id = store_->issue_transient();
        if (id == core::kNoOrderId) [[unlikely]] {
            return rejected(RejectReason::BookFull);
        }
    }

    const core::Qty filled = match(order.side, limit, id, order.qty, sink);
    const core::Qty left = order.qty - filled;
    core::Qty resting{0};
    if (slot != mem::kNullIndex) {
        if (left > core::Qty{0}) {
            rest(slot, order, limit, left);
            resting = left;
        } else {
            store_->release(slot);
        }
    }

    PLANCK_ENSURE(filled.raw() <= order.qty.raw());
    return AddResult{.id = id, .filled = filled, .resting = resting, .reject = RejectReason::None};
}

template <TradeSink S>
core::Qty OrderBook::match(core::Side side, Offset limit, core::OrderId aggressor, core::Qty want,
                           S& sink) noexcept {
    PLANCK_EXPECT(limit < band_ticks_);
    PLANCK_EXPECT(want > core::Qty{0});

    const bool buy = (side == core::Side::Buy);
    Offset& best = buy ? best_ask_ : best_bid_;
    core::Qty left = want;

    // Each pass either removes one resting order or finishes the aggressor,
    // so the store's capacity bounds the loop (Power of Ten, rule 2).
    std::uint32_t removals_left = store_->capacity();
    while (left > core::Qty{0} && best != kNoLevel && crosses(buy, limit, best) &&
           removals_left > 0) {
        const Offset offset = best;
        Level& level = levels_[offset];
        PLANCK_ASSERT(level.head != mem::kNullIndex);

        const mem::Index slot = level.head;
        Order& resting = (*store_)[slot];
        if (resting.next != mem::kNullIndex) {
            // The next order in the queue is the likely next fill.
            core::prefetch_write(&(*store_)[resting.next]);
        }

        const core::Qty fill = std::min(left, resting.qty);
        PLANCK_ASSERT(fill > core::Qty{0} && level.total >= fill.raw());
        left -= fill;
        resting.qty -= fill;
        level.total -= fill.raw();

        sink.on_trade(Trade{.aggressor = aggressor,
                            .resting = resting.id,
                            .price = resting.price,
                            .qty = fill,
                            .resting_leaves = resting.qty,
                            .resting_owner = resting.owner,
                            .aggressor_side = side});

        if (resting.qty == core::Qty{0}) {
            pop_filled_head(buy, offset, level, slot, resting);
            --removals_left;
        }
    }

    PLANCK_ENSURE(left.raw() <= want.raw());
    return want - left;
}

inline void OrderBook::pop_filled_head(bool aggressor_is_buy, Offset offset, Level& level,
                                       mem::Index slot, const Order& filled) noexcept {
    PLANCK_EXPECT(level.head == slot);
    PLANCK_EXPECT(filled.qty == core::Qty{0});

    level.head = filled.next;
    if (level.head == mem::kNullIndex) {
        PLANCK_ASSERT(level.total == 0);
        level.tail = mem::kNullIndex;
        // A buy consumes asks, so the emptied level is a bid only on a sell.
        level_emptied(!aggressor_is_buy, offset);
    } else {
        (*store_)[level.head].prev = mem::kNullIndex;
    }
    store_->release(slot);
}

inline void OrderBook::rest(mem::Index slot, const NewOrder& incoming, Offset offset,
                            core::Qty qty) noexcept {
    PLANCK_EXPECT(offset < band_ticks_);
    PLANCK_EXPECT(qty > core::Qty{0});

    const bool buy = (incoming.side == core::Side::Buy);
    // Matching has already removed everything this order could trade with,
    // so resting it cannot cross the book.
    PLANCK_ASSERT(buy ? (best_ask_ == kNoLevel || best_ask_ > offset)
                      : (best_bid_ == kNoLevel || best_bid_ < offset));

    Order& order = (*store_)[slot];
    Level& level = levels_[offset];
    order.next = mem::kNullIndex;
    order.prev = level.tail;
    order.qty = qty;
    order.price = incoming.price;
    order.owner = incoming.owner;
    order.symbol = symbol_;
    order.side = incoming.side;

    if (level.tail == mem::kNullIndex) {
        level.head = slot;
        if (buy) {
            bids_.set(offset);
            best_bid_ = (best_bid_ == kNoLevel) ? offset : std::max(best_bid_, offset);
        } else {
            asks_.set(offset);
            best_ask_ = (best_ask_ == kNoLevel) ? offset : std::min(best_ask_, offset);
        }
    } else {
        (*store_)[level.tail].next = slot;
    }
    level.tail = slot;
    level.total += qty.raw();
}

inline CancelResult OrderBook::cancel(core::OrderId id) noexcept {
    PLANCK_EXPECT(initialised());

    const mem::Index slot = store_->find(id);
    if (slot == mem::kNullIndex) [[unlikely]] {
        return CancelResult{.reject = RejectReason::UnknownOrder};
    }
    const Order& order = (*store_)[slot];
    // The store is shared by every book in the shard, so the id may be live
    // but belong to another symbol.
    if (order.symbol != symbol_) [[unlikely]] {
        return CancelResult{.reject = RejectReason::UnknownOrder};
    }

    Offset offset = 0;
    const bool in_range = try_offset(order.price, offset);
    PLANCK_ASSERT(in_range);
    static_cast<void>(in_range);
    Level& level = levels_[offset];
    PLANCK_ASSERT(level.total >= order.qty.raw());

    // Unlink from the level's queue.
    if (order.prev == mem::kNullIndex) {
        level.head = order.next;
    } else {
        (*store_)[order.prev].next = order.next;
    }
    if (order.next == mem::kNullIndex) {
        level.tail = order.prev;
    } else {
        (*store_)[order.next].prev = order.prev;
    }
    level.total -= order.qty.raw();

    const CancelResult result{.cancelled = order.qty,
                              .price = order.price,
                              .side = order.side,
                              .reject = RejectReason::None};
    if (level.head == mem::kNullIndex) {
        PLANCK_ASSERT(level.total == 0);
        level_emptied(order.side == core::Side::Buy, offset);
    }
    store_->release(slot);
    return result;
}

}  // namespace planck::book

#endif  // PLANCK_BOOK_ORDER_BOOK_HPP
