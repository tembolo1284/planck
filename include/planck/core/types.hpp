#ifndef PLANCK_CORE_TYPES_HPP
#define PLANCK_CORE_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <utility>

#include "planck/core/contract.hpp"
#include "planck/core/strong.hpp"

namespace planck::core {

struct PriceTag {
    static constexpr bool arithmetic = true;
};
struct QtyTag {
    static constexpr bool arithmetic = true;
};
struct NanosTag {
    static constexpr bool arithmetic = true;
};
struct OrderIdTag {};
struct ClientOrderIdTag {};
struct SymbolIdTag {};
struct SessionIdTag {};
struct ShardIdTag {};
struct SeqNoTag {};

// Price in integer ticks. Signed, because spreads and some futures go negative.
using Price = Strong<PriceTag, std::int32_t>;
// Quantity in lots.
using Qty = Strong<QtyTag, std::uint32_t>;
// Nanoseconds. Either a duration or time since the Unix epoch.
using Nanos = Strong<NanosTag, std::uint64_t>;

// Assigned by the engine. Never reused within a trading day.
using OrderId = Strong<OrderIdTag, std::uint64_t>;
// Chosen by the client. Unique per session only.
using ClientOrderId = Strong<ClientOrderIdTag, std::uint64_t>;
// Dense index into the symbol table, below kMaxSymbols.
using SymbolId = Strong<SymbolIdTag, std::uint16_t>;
// Dense index into the session table, below kMaxSessions.
using SessionId = Strong<SessionIdTag, std::uint16_t>;
// Index of a matching thread, below kMaxShards.
using ShardId = Strong<ShardIdTag, std::uint8_t>;
// Position in a sequenced stream. Starts at 1, so 0 means "none".
using SeqNo = Strong<SeqNoTag, std::uint64_t>;

inline constexpr OrderId kNoOrderId{0};
inline constexpr SeqNo kNoSeqNo{0};

static_assert(StrongType<Price> && sizeof(Price) == 4);
static_assert(StrongType<Qty> && sizeof(Qty) == 4);
static_assert(StrongType<Nanos> && sizeof(Nanos) == 8);
static_assert(StrongType<OrderId> && sizeof(OrderId) == 8);
static_assert(StrongType<ClientOrderId> && sizeof(ClientOrderId) == 8);
static_assert(StrongType<SymbolId> && sizeof(SymbolId) == 2);
static_assert(StrongType<SessionId> && sizeof(SessionId) == 2);
static_assert(StrongType<ShardId> && sizeof(ShardId) == 1);
static_assert(StrongType<SeqNo> && sizeof(SeqNo) == 8);

// The enums below arrive as raw bytes off the wire. Run is_valid() on them
// at the boundary. Everything past the boundary may assume they are in range.

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
inline constexpr std::size_t kSideCount = 2;

enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };
inline constexpr std::size_t kOrderTypeCount = 2;

enum class TimeInForce : std::uint8_t { Day = 0, ImmediateOrCancel = 1, FillOrKill = 2 };
inline constexpr std::size_t kTimeInForceCount = 3;

[[nodiscard]] constexpr bool is_valid(Side side) noexcept {
    return std::to_underlying(side) < kSideCount;
}

[[nodiscard]] constexpr bool is_valid(OrderType type) noexcept {
    return std::to_underlying(type) < kOrderTypeCount;
}

[[nodiscard]] constexpr bool is_valid(TimeInForce tif) noexcept {
    return std::to_underlying(tif) < kTimeInForceCount;
}

// Array index for per-side tables: Buy is 0, Sell is 1.
[[nodiscard]] constexpr std::size_t index_of(Side side) noexcept {
    PLANCK_EXPECT(is_valid(side));
    return std::to_underlying(side);
}

// Branchless flip.
[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    PLANCK_EXPECT(is_valid(side));
    return static_cast<Side>(std::to_underlying(side) ^ std::uint8_t{1});
}

}  // namespace planck::core

#endif  // PLANCK_CORE_TYPES_HPP
