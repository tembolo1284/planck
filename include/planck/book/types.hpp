#ifndef PLANCK_BOOK_TYPES_HPP
#define PLANCK_BOOK_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "planck/core/types.hpp"

namespace planck::book {

// Why the book turned an order or a cancel away. A business outcome, not a
// system error, so it is separate from core::Status.
enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidQuantity,  // quantity was zero
    PriceOutOfBand,   // limit price outside the book's price band
    NotFillable,      // fill-or-kill could not be filled in full
    BookFull,         // no free order slot, or order ids exhausted
    UnknownOrder,     // cancel named an order that is not resting here
};

inline constexpr std::size_t kRejectReasonCount = 6;

// Static string. "Unknown" for a value outside the enum.
[[nodiscard]] std::string_view to_string(RejectReason reason) noexcept;

// An incoming order. Enum fields must already be validated. price is ignored
// for market orders. A market order never rests, whatever its time in force.
struct NewOrder {
    core::Side side = core::Side::Buy;
    core::OrderType type = core::OrderType::Limit;
    core::TimeInForce tif = core::TimeInForce::Day;
    core::Price price{};
    core::Qty qty{};
};

// Outcome of OrderBook::add. If reject is None then
// filled + resting <= requested quantity, and whatever is left over was
// cancelled (IOC remainder or unfilled market order).
struct AddResult {
    core::OrderId id = core::kNoOrderId;
    core::Qty filled{};
    core::Qty resting{};
    RejectReason reject = RejectReason::None;
};

// Outcome of OrderBook::cancel. side and price describe the order removed.
struct CancelResult {
    core::Qty cancelled{};
    core::Price price{};
    core::Side side = core::Side::Buy;
    RejectReason reject = RejectReason::None;
};

// One fill. Always executes at the resting order's price.
struct Trade {
    core::OrderId aggressor = core::kNoOrderId;
    core::OrderId resting = core::kNoOrderId;
    core::Price price{};
    core::Qty qty{};
    // What the resting order has left. Zero means it has left the book.
    core::Qty resting_leaves{};
    core::Side aggressor_side = core::Side::Buy;
};

// Receives fills as they happen, in execution order. Passed to add() as a
// template parameter so the call inlines. No virtual dispatch on the hot path.
template <typename S>
concept TradeSink = requires(S& sink, const Trade& trade) {
    { sink.on_trade(trade) } noexcept;
};

// Best price on one side and the total quantity resting there.
struct Quote {
    core::Price price{};
    std::uint64_t qty = 0;
    bool valid = false;
};

}  // namespace planck::book

#endif  // PLANCK_BOOK_TYPES_HPP
