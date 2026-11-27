#ifndef PLANCK_WIRE_ORDER_ENTRY_HPP
#define PLANCK_WIRE_ORDER_ENTRY_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "planck/wire/types.hpp"

namespace planck::wire {

// Order entry runs over TCP, one session per connection. The client logs in,
// then sends NewOrder and CancelOrder. The venue answers every NewOrder with
// exactly one OrderAccepted or OrderRejected, then zero or more Executed, and
// a Canceled if the order leaves the book unfilled.
//
// Either side sends Heartbeat when it has had nothing else to say for a
// while, so that the other can tell a quiet line from a dead one. Nobody
// answers a Heartbeat. LoginAccepted tells the client how long the venue
// will put up with hearing nothing from it.
//
// The venue numbers the reports it sends a session: OrderAccepted,
// OrderRejected, Executed, Canceled and CancelRejected, once the session
// has logged in. The numbers start at 1 and go up by one, across however
// many connections the session uses. They are not on the wire. TCP loses
// nothing and reorders nothing within a connection, so LoginAccepted says
// what number the next report has and the client counts from there. A
// client that reconnects says what number it expects next, and is sent
// what it missed before anything new. See LoginRequest and LoginAccepted.
//
// Each struct's default value already has a correct header, so
//     NewOrder order{};
// is ready to fill in and send.

// ---- session ----

struct LoginRequest {
    static constexpr MsgType kType = MsgType::LoginRequest;
    MsgHeader header{sizeof(LoginRequest), kType, 0};
    std::uint16_t protocol_version = kProtocolVersion;
    std::uint16_t reserved = 0;
    std::uint32_t client_id = 0;
    std::uint32_t reserved2 = 0;
    std::uint64_t token = 0;
    // Which run of the venue next_report was counted in: the epoch from the
    // LoginAccepted of an earlier connection. Zero if there was none.
    std::uint64_t epoch = 0;
    // The number of the first report the client has not seen. Zero: it wants
    // nothing from before, only what is new. 1: everything the venue still
    // holds. If the epoch is not the venue's own the venue has restarted and
    // the number means nothing to it: the client is then sent everything
    // held, as for 1.
    std::uint64_t next_report = 0;
};

struct LoginAccepted {
    static constexpr MsgType kType = MsgType::LoginAccepted;
    MsgHeader header{sizeof(LoginAccepted), kType, 0};
    std::uint16_t session_id = 0;
    // The venue ends a connection it has heard nothing on for this long, in
    // milliseconds. Zero means it never does. A client should send something,
    // a Heartbeat if nothing else, about three times as often.
    std::uint16_t idle_timeout_ms = 0;
    // This run of the venue. Report numbers start again at 1 when it changes.
    std::uint64_t epoch = 0;
    // The number of the first report this connection will carry. Above what
    // was asked for means the reports in between are no longer held.
    std::uint64_t next_report = 0;
    // The number the first new report will have. Those from next_report up
    // to here are ones from before, sent again or sent late.
    std::uint64_t live_from = 0;
    // The highest client order id the venue has had from this session. A
    // NewOrder whose id is not above it is taken for a repeat and ignored.
    std::uint64_t last_client_order_id = 0;
};

struct LoginRejected {
    static constexpr MsgType kType = MsgType::LoginRejected;
    MsgHeader header{sizeof(LoginRejected), kType, 0};
    LoginRejectReason reason = LoginRejectReason::BadCredentials;
    std::array<std::uint8_t, 3> reserved{};
};

struct Heartbeat {
    static constexpr MsgType kType = MsgType::Heartbeat;
    MsgHeader header{sizeof(Heartbeat), kType, 0};
};

struct Logout {
    static constexpr MsgType kType = MsgType::Logout;
    MsgHeader header{sizeof(Logout), kType, 0};
};

// ---- client to venue ----

// side, order_type and tif carry the numeric values of core::Side,
// core::OrderType and core::TimeInForce. They are raw bytes here because
// nothing from the network is trusted until wire::well_formed() has seen it.
struct NewOrder {
    static constexpr MsgType kType = MsgType::NewOrder;
    MsgHeader header{sizeof(NewOrder), kType, 0};
    std::uint16_t symbol_id = 0;
    std::uint8_t side = 0;
    std::uint8_t order_type = 0;
    // Chosen by the client and echoed back. It must be above every id the
    // session has sent before, on any connection: an order whose id is not
    // is taken for a repeat of one the venue already has, and is ignored
    // without an answer. That makes sending an order again safe.
    std::uint64_t client_order_id = 0;
    // Ticks. Ignored for market orders.
    std::int32_t price = 0;
    std::uint32_t qty = 0;
    std::uint8_t tif = 0;
    std::array<std::uint8_t, 7> reserved{};
};

// Cancels by the venue's order id from OrderAccepted, which the venue can
// resolve with one array read. There is no cancel by client order id.
struct CancelOrder {
    static constexpr MsgType kType = MsgType::CancelOrder;
    MsgHeader header{sizeof(CancelOrder), kType, 0};
    std::uint32_t reserved = 0;
    std::uint64_t order_id = 0;
};

// ---- venue to client ----

struct OrderAccepted {
    static constexpr MsgType kType = MsgType::OrderAccepted;
    MsgHeader header{sizeof(OrderAccepted), kType, 0};
    std::uint32_t reserved = 0;
    std::uint64_t client_order_id = 0;
    // The venue's id for this order. Use it to cancel.
    std::uint64_t order_id = 0;
    // Nanoseconds since the Unix epoch, taken by the matching thread.
    std::uint64_t timestamp = 0;
};

struct OrderRejected {
    static constexpr MsgType kType = MsgType::OrderRejected;
    MsgHeader header{sizeof(OrderRejected), kType, 0};
    RejectCode reason = RejectCode::None;
    std::array<std::uint8_t, 3> reserved{};
    std::uint64_t client_order_id = 0;
};

// One fill. Sent to both parties of the trade, each with its own order_id
// and liquidity flag and the same match_id.
struct Executed {
    static constexpr MsgType kType = MsgType::Executed;
    MsgHeader header{sizeof(Executed), kType, 0};
    Liquidity liquidity = Liquidity::Maker;
    std::array<std::uint8_t, 3> reserved{};
    std::uint64_t order_id = 0;
    std::uint64_t match_id = 0;
    std::int32_t price = 0;
    std::uint32_t qty = 0;
    // Open quantity after this fill. Zero means the order is done.
    std::uint32_t leaves = 0;
    std::uint32_t reserved2 = 0;
    std::uint64_t timestamp = 0;
};

struct Canceled {
    static constexpr MsgType kType = MsgType::Canceled;
    MsgHeader header{sizeof(Canceled), kType, 0};
    CancelReason reason = CancelReason::UserRequested;
    std::array<std::uint8_t, 3> reserved{};
    std::uint64_t order_id = 0;
    // Quantity removed from the book.
    std::uint32_t qty = 0;
    std::uint32_t reserved2 = 0;
    std::uint64_t timestamp = 0;
};

struct CancelRejected {
    static constexpr MsgType kType = MsgType::CancelRejected;
    MsgHeader header{sizeof(CancelRejected), kType, 0};
    RejectCode reason = RejectCode::None;
    std::array<std::uint8_t, 3> reserved{};
    std::uint64_t order_id = 0;
};

// The layout is the protocol. These pin it down so an accidental change to a
// struct fails the build instead of silently breaking every client.
static_assert(Message<LoginRequest> && sizeof(LoginRequest) == 40);
static_assert(offsetof(LoginRequest, client_id) == 8 && offsetof(LoginRequest, token) == 16);
static_assert(offsetof(LoginRequest, epoch) == 24 && offsetof(LoginRequest, next_report) == 32);
static_assert(Message<LoginAccepted> && sizeof(LoginAccepted) == 40);
static_assert(offsetof(LoginAccepted, epoch) == 8 && offsetof(LoginAccepted, next_report) == 16);
static_assert(offsetof(LoginAccepted, live_from) == 24);
static_assert(offsetof(LoginAccepted, last_client_order_id) == 32);
static_assert(Message<LoginRejected> && sizeof(LoginRejected) == 8);
static_assert(Message<Heartbeat> && sizeof(Heartbeat) == 4);
static_assert(Message<Logout> && sizeof(Logout) == 4);

static_assert(Message<NewOrder> && sizeof(NewOrder) == 32);
static_assert(offsetof(NewOrder, symbol_id) == 4 && offsetof(NewOrder, side) == 6);
static_assert(offsetof(NewOrder, order_type) == 7 && offsetof(NewOrder, client_order_id) == 8);
static_assert(offsetof(NewOrder, price) == 16 && offsetof(NewOrder, qty) == 20);
static_assert(offsetof(NewOrder, tif) == 24);
static_assert(Message<CancelOrder> && sizeof(CancelOrder) == 16);
static_assert(offsetof(CancelOrder, order_id) == 8);

static_assert(Message<OrderAccepted> && sizeof(OrderAccepted) == 32);
static_assert(offsetof(OrderAccepted, client_order_id) == 8);
static_assert(offsetof(OrderAccepted, order_id) == 16 && offsetof(OrderAccepted, timestamp) == 24);
static_assert(Message<OrderRejected> && sizeof(OrderRejected) == 16);
static_assert(offsetof(OrderRejected, client_order_id) == 8);
static_assert(Message<Executed> && sizeof(Executed) == 48);
static_assert(offsetof(Executed, order_id) == 8 && offsetof(Executed, match_id) == 16);
static_assert(offsetof(Executed, price) == 24 && offsetof(Executed, qty) == 28);
static_assert(offsetof(Executed, leaves) == 32 && offsetof(Executed, timestamp) == 40);
static_assert(Message<Canceled> && sizeof(Canceled) == 32);
static_assert(offsetof(Canceled, order_id) == 8 && offsetof(Canceled, qty) == 16);
static_assert(offsetof(Canceled, timestamp) == 24);
static_assert(Message<CancelRejected> && sizeof(CancelRejected) == 16);
static_assert(offsetof(CancelRejected, order_id) == 8);

}  // namespace planck::wire

#endif  // PLANCK_WIRE_ORDER_ENTRY_HPP
