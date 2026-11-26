#include "planck/book/order_book.hpp"

#include <limits>
#include <span>

#include "planck/core/bits.hpp"

namespace planck::book {

using core::Status;

Status OrderBook::init(const BookConfig& config, OrderStore& store, mem::Arena& arena) noexcept {
    if (initialised()) {
        return Status::Duplicate;
    }
    if (config.band_ticks < kMinBandTicks || config.band_ticks > kMaxBandTicks ||
        !core::is_pow2(config.band_ticks)) {
        return Status::InvalidArgument;
    }
    // The top of the band must still be a representable price.
    const std::int64_t band_high =
        static_cast<std::int64_t>(config.band_low.raw()) + (config.band_ticks - 1);
    if (band_high > std::numeric_limits<std::int32_t>::max()) {
        return Status::OutOfRange;
    }
    if (store.capacity() == 0) {
        return Status::InvalidArgument;
    }

    const std::size_t words = config.band_ticks / 64U;
    const std::span<Level> levels = arena.allocate_array<Level>(config.band_ticks);
    const std::span<std::uint64_t> bid_words = arena.allocate_array<std::uint64_t>(words);
    const std::span<std::uint64_t> ask_words = arena.allocate_array<std::uint64_t>(words);
    if (levels.empty() || bid_words.empty() || ask_words.empty()) {
        return Status::Capacity;
    }

    if (const Status status = bids_.init(bid_words, config.band_ticks); !core::ok(status)) {
        return status;
    }
    if (const Status status = asks_.init(ask_words, config.band_ticks); !core::ok(status)) {
        return status;
    }

    // Arena memory is all zero, but zero is a valid slot index. Every level
    // must start out with the "no order" marker instead.
    for (Level& level : levels) {
        level = Level{};
    }

    levels_ = levels.data();
    store_ = &store;
    best_bid_ = kNoLevel;
    best_ask_ = kNoLevel;
    band_ticks_ = config.band_ticks;
    band_low_ = config.band_low;
    symbol_ = config.symbol;

    PLANCK_ENSURE(initialised());
    return Status::Ok;
}

bool OrderBook::can_fill(core::Side side, Offset limit, core::Qty want) const noexcept {
    PLANCK_EXPECT(limit < band_ticks_);
    PLANCK_EXPECT(want > core::Qty{0});

    const bool buy = (side == core::Side::Buy);
    std::uint64_t need = want.raw();
    Offset level = buy ? best_ask_ : best_bid_;

    // Visits each occupied level at most once, so the band size bounds it.
    std::uint32_t levels_left = band_ticks_;
    while (level != kNoLevel && crosses(buy, limit, level) && levels_left > 0) {
        const std::uint64_t available = levels_[level].total;
        if (available >= need) {
            return true;
        }
        need -= available;
        --levels_left;

        if (buy) {
            if ((level + 1) >= band_ticks_) {
                break;
            }
            level = asks_.lowest_from(level + 1);
        } else {
            if (level == 0) {
                break;
            }
            level = bids_.highest_upto(level - 1);
        }
    }
    return false;
}

}  // namespace planck::book
