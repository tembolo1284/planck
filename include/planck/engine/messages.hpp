#ifndef PLANCK_ENGINE_MESSAGES_HPP
#define PLANCK_ENGINE_MESSAGES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "planck/core/config.hpp"
#include "planck/core/types.hpp"
#include "planck/wire/codec.hpp"

namespace planck::engine {

// What travels through the rings between a gateway, a shard and the feed.
// All three types are fixed-size plain data.

enum class CommandKind : std::uint8_t { NewOrder = 0, CancelOrder = 1 };

// A request for a shard. The gateway builds one only from a wire message
// that has passed wire::well_formed(), so the shard trusts every field.
// 32 bytes: two per cache line.
struct Command {
    // Cycle counter reading taken when the request came off the socket. It
    // rides along into every Report this command causes, so the far end can
    // measure time through the engine.
    std::uint64_t ingress_tsc = 0;
    // NewOrder: the client's order id. CancelOrder: the venue order id.
    std::uint64_t id = 0;
    core::Price price{};
    core::Qty qty{};
    core::SymbolId symbol{};
    core::SessionId session{};
    CommandKind kind = CommandKind::NewOrder;
    core::Side side = core::Side::Buy;
    core::OrderType type = core::OrderType::Limit;
    core::TimeInForce tif = core::TimeInForce::Day;
};

static_assert(sizeof(Command) == 32);
static_assert(std::is_trivially_copyable_v<Command>);

[[nodiscard]] inline Command make_command(const wire::NewOrder& order, core::SessionId session,
                                          std::uint64_t ingress_tsc) noexcept {
    return Command{.ingress_tsc = ingress_tsc,
                   .id = order.client_order_id,
                   .price = core::Price{order.price},
                   .qty = core::Qty{order.qty},
                   .symbol = core::SymbolId{order.symbol_id},
                   .session = session,
                   .kind = CommandKind::NewOrder,
                   .side = wire::side_of(order),
                   .type = wire::order_type_of(order),
                   .tif = wire::tif_of(order)};
}

[[nodiscard]] inline Command make_command(const wire::CancelOrder& cancel, core::SessionId session,
                                          std::uint64_t ingress_tsc) noexcept {
    return Command{.ingress_tsc = ingress_tsc,
                   .id = cancel.order_id,
                   .session = session,
                   .kind = CommandKind::CancelOrder};
}

// A message for one order entry session. The shard has already encoded it:
// bytes[0, length) is a complete wire message, and the gateway only has to
// copy it to the right socket. 64 bytes: one cache line.
struct Report {
    std::uint64_t ingress_tsc = 0;
    core::SessionId session{};
    std::uint16_t length = 0;
    std::uint32_t reserved = 0;
    std::array<std::byte, wire::kMaxMessageSize> bytes{};
};

static_assert(sizeof(Report) == core::kCacheLine);
static_assert(std::is_trivially_copyable_v<Report>);

// Largest market data message.
inline constexpr std::size_t kMaxMarketMessageSize = 40;
static_assert(sizeof(wire::MdAddOrder) <= kMaxMarketMessageSize);
static_assert(sizeof(wire::MdExecuted) <= kMaxMarketMessageSize);
static_assert(sizeof(wire::MdDeleted) <= kMaxMarketMessageSize);

// A message for the public feed, also already encoded. 48 bytes.
struct MarketEvent {
    std::uint16_t length = 0;
    std::uint16_t reserved = 0;
    std::uint32_t reserved2 = 0;
    std::array<std::byte, kMaxMarketMessageSize> bytes{};
};

static_assert(sizeof(MarketEvent) == 48);
static_assert(std::is_trivially_copyable_v<MarketEvent>);

}  // namespace planck::engine

#endif  // PLANCK_ENGINE_MESSAGES_HPP
