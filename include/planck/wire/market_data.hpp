#ifndef PLANCK_WIRE_MARKET_DATA_HPP
#define PLANCK_WIRE_MARKET_DATA_HPP

#include <cstddef>
#include <cstdint>

#include "planck/wire/types.hpp"

namespace planck::wire {

// Market data goes out as UDP multicast, one stream per matching shard. It is
// an order-by-order feed: a subscriber that applies every message in sequence
// holds an exact copy of each book.
//
//   MdAddOrder  an order started resting
//   MdExecuted  a resting order traded, in part or in full
//   MdDeleted   a resting order was cancelled
//
// An order whose open quantity reaches zero through MdExecuted is gone. No
// MdDeleted follows. Orders that trade without ever resting appear only as
// the MdExecuted of whatever they hit.
//
// A datagram is one PacketHeader followed by `count` messages back to back.

// Stay under a 1500-byte Ethernet MTU after IP and UDP headers, with slack
// for tunnels. A datagram that fragments is a datagram that gets lost.
inline constexpr std::size_t kMaxDatagramSize = 1400;

struct PacketHeader {
    // Sequence number of the first message in this datagram. Each following
    // message is one higher. Starts at 1 for the day. A subscriber that sees
    // a jump has lost messages and must recover.
    std::uint64_t sequence = 0;
    // Messages in this datagram. Zero is a heartbeat: sequence then holds
    // the number the next real message will carry.
    std::uint16_t count = 0;
    // Which shard's stream this is.
    std::uint16_t stream_id = 0;
    std::uint32_t reserved = 0;
};

struct MdAddOrder {
    static constexpr MsgType kType = MsgType::MdAddOrder;
    MsgHeader header{sizeof(MdAddOrder), kType, 0};
    std::uint16_t symbol_id = 0;
    std::uint8_t side = 0;  // numeric value of core::Side
    std::uint8_t reserved = 0;
    std::uint64_t order_id = 0;
    std::int32_t price = 0;
    std::uint32_t qty = 0;
    std::uint64_t timestamp = 0;
};

struct MdExecuted {
    static constexpr MsgType kType = MsgType::MdExecuted;
    MsgHeader header{sizeof(MdExecuted), kType, 0};
    std::uint16_t symbol_id = 0;
    std::uint16_t reserved = 0;
    // The resting order that was hit.
    std::uint64_t order_id = 0;
    std::uint64_t match_id = 0;
    // Quantity traded. Subtract it from the order's open quantity.
    std::uint32_t qty = 0;
    // Always the resting order's price. Repeated here so a trade print needs
    // no book lookup.
    std::int32_t price = 0;
    std::uint64_t timestamp = 0;
};

struct MdDeleted {
    static constexpr MsgType kType = MsgType::MdDeleted;
    MsgHeader header{sizeof(MdDeleted), kType, 0};
    std::uint16_t symbol_id = 0;
    std::uint16_t reserved = 0;
    std::uint64_t order_id = 0;
    std::uint64_t timestamp = 0;
};

static_assert(sizeof(PacketHeader) == 16 && alignof(PacketHeader) == 8);
static_assert(std::has_unique_object_representations_v<PacketHeader>);
static_assert(offsetof(PacketHeader, count) == 8 && offsetof(PacketHeader, stream_id) == 10);

static_assert(Message<MdAddOrder> && sizeof(MdAddOrder) == 32);
static_assert(offsetof(MdAddOrder, symbol_id) == 4 && offsetof(MdAddOrder, side) == 6);
static_assert(offsetof(MdAddOrder, order_id) == 8 && offsetof(MdAddOrder, price) == 16);
static_assert(offsetof(MdAddOrder, qty) == 20 && offsetof(MdAddOrder, timestamp) == 24);
static_assert(Message<MdExecuted> && sizeof(MdExecuted) == 40);
static_assert(offsetof(MdExecuted, order_id) == 8 && offsetof(MdExecuted, match_id) == 16);
static_assert(offsetof(MdExecuted, qty) == 24 && offsetof(MdExecuted, price) == 28);
static_assert(offsetof(MdExecuted, timestamp) == 32);
static_assert(Message<MdDeleted> && sizeof(MdDeleted) == 24);
static_assert(offsetof(MdDeleted, order_id) == 8 && offsetof(MdDeleted, timestamp) == 16);

}  // namespace planck::wire

#endif  // PLANCK_WIRE_MARKET_DATA_HPP
