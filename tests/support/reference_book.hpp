#ifndef PLANCK_TESTS_SUPPORT_REFERENCE_BOOK_HPP
#define PLANCK_TESTS_SUPPORT_REFERENCE_BOOK_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "planck/book/types.hpp"
#include "planck/core/types.hpp"

namespace planck::test {

struct ReferenceFill {
    std::uint64_t resting_id = 0;
    std::int32_t price = 0;
    std::uint32_t qty = 0;
    std::uint32_t resting_leaves = 0;
    std::uint32_t resting_owner = 0;
};

struct ReferenceAdd {
    book::RejectReason reject = book::RejectReason::None;
    std::uint32_t filled = 0;
    // Nonzero means the remainder should rest. The caller then calls rest()
    // with the id the real book assigned.
    std::uint32_t resting = 0;
    std::vector<ReferenceFill> fills;
};

struct ReferenceCancel {
    bool found = false;
    std::uint32_t qty = 0;
    std::int32_t price = 0;
    core::Side side = core::Side::Buy;
};

// A limit order book written to be obviously correct and nothing else:
// ordered maps of deques, linear scans, no cleverness. It shares no code and
// no data structure with book::OrderBook, so when the two agree on a long
// random run, that agreement means something.
class ReferenceBook {
public:
    ReferenceBook(std::int32_t band_low, std::uint32_t band_ticks)
        : band_low_(band_low), band_high_(band_low + static_cast<std::int32_t>(band_ticks) - 1) {}

    // store_has_room: whether the shared order store could take one more
    // resting order. The reference has no store of its own.
    ReferenceAdd add(const book::NewOrder& order, bool store_has_room) {
        ReferenceAdd result;
        const bool buy = (order.side == core::Side::Buy);
        const bool market = (order.type == core::OrderType::Market);
        const std::uint32_t qty = order.qty.raw();

        if (qty == 0) {
            result.reject = book::RejectReason::InvalidQuantity;
            return result;
        }
        std::int32_t limit = buy ? band_high_ : band_low_;
        if (!market) {
            if (order.price.raw() < band_low_ || order.price.raw() > band_high_) {
                result.reject = book::RejectReason::PriceOutOfBand;
                return result;
            }
            limit = order.price.raw();
        }
        if (order.tif == core::TimeInForce::FillOrKill && crossable(buy, limit) < qty) {
            result.reject = book::RejectReason::NotFillable;
            return result;
        }
        const bool may_rest = !market && (order.tif == core::TimeInForce::Day);
        if (may_rest && !store_has_room) {
            result.reject = book::RejectReason::BookFull;
            return result;
        }

        std::uint32_t left = qty;
        Levels& opposite = buy ? asks_ : bids_;
        while (left > 0 && !opposite.empty()) {
            const auto level = buy ? opposite.begin() : std::prev(opposite.end());
            const bool crosses = buy ? (level->first <= limit) : (level->first >= limit);
            if (!crosses) {
                break;
            }
            take(level->second, level->first, left, result.fills);
            if (level->second.empty()) {
                opposite.erase(level);
            }
        }

        result.filled = qty - left;
        result.resting = may_rest ? left : 0;
        return result;
    }

    void rest(std::uint64_t id, const book::NewOrder& order, std::uint32_t qty) {
        Levels& side = (order.side == core::Side::Buy) ? bids_ : asks_;
        side[order.price.raw()].push_back(Resting{id, qty, order.owner});
        where_[id] = {order.side, order.price.raw()};
    }

    ReferenceCancel cancel(std::uint64_t id) {
        const auto found = where_.find(id);
        if (found == where_.end()) {
            return {};
        }
        const auto [side, price] = found->second;
        Levels& levels = (side == core::Side::Buy) ? bids_ : asks_;
        std::deque<Resting>& queue = levels[price];
        const auto order = std::find_if(queue.begin(), queue.end(),
                                        [id](const Resting& r) { return r.id == id; });
        const ReferenceCancel result{true, order->qty, price, side};
        queue.erase(order);
        if (queue.empty()) {
            levels.erase(price);
        }
        where_.erase(found);
        return result;
    }

    [[nodiscard]] bool has(std::uint64_t id) const { return where_.contains(id); }
    [[nodiscard]] std::size_t live() const { return where_.size(); }

    [[nodiscard]] book::Quote best(core::Side side) const {
        const Levels& levels = (side == core::Side::Buy) ? bids_ : asks_;
        if (levels.empty()) {
            return {};
        }
        const auto level = (side == core::Side::Buy) ? std::prev(levels.end()) : levels.begin();
        return book::Quote{core::Price{level->first}, total(level->second), true};
    }

    [[nodiscard]] std::uint64_t quantity_at(std::int32_t price) const {
        std::uint64_t sum = 0;
        if (const auto level = bids_.find(price); level != bids_.end()) {
            sum += total(level->second);
        }
        if (const auto level = asks_.find(price); level != asks_.end()) {
            sum += total(level->second);
        }
        return sum;
    }

private:
    struct Resting {
        std::uint64_t id = 0;
        std::uint32_t qty = 0;
        std::uint32_t owner = 0;
    };
    using Levels = std::map<std::int32_t, std::deque<Resting>>;

    static std::uint64_t total(const std::deque<Resting>& queue) {
        std::uint64_t sum = 0;
        for (const Resting& order : queue) {
            sum += order.qty;
        }
        return sum;
    }

    // Quantity an order with this limit could trade against right now.
    [[nodiscard]] std::uint64_t crossable(bool buy, std::int32_t limit) const {
        std::uint64_t sum = 0;
        for (const auto& [price, queue] : (buy ? asks_ : bids_)) {
            if (buy ? (price <= limit) : (price >= limit)) {
                sum += total(queue);
            }
        }
        return sum;
    }

    // Fill from the front of one price level until it or the taker runs out.
    void take(std::deque<Resting>& queue, std::int32_t price, std::uint32_t& left,
              std::vector<ReferenceFill>& fills) {
        while (left > 0 && !queue.empty()) {
            Resting& resting = queue.front();
            const std::uint32_t fill = std::min(left, resting.qty);
            left -= fill;
            resting.qty -= fill;
            fills.push_back(ReferenceFill{resting.id, price, fill, resting.qty, resting.owner});
            if (resting.qty == 0) {
                where_.erase(resting.id);
                queue.pop_front();
            }
        }
    }

    std::int32_t band_low_;
    std::int32_t band_high_;
    Levels bids_;
    Levels asks_;
    std::unordered_map<std::uint64_t, std::pair<core::Side, std::int32_t>> where_;
};

}  // namespace planck::test

#endif  // PLANCK_TESTS_SUPPORT_REFERENCE_BOOK_HPP
