#ifndef PLANCK_GATEWAY_GATEWAY_HPP
#define PLANCK_GATEWAY_GATEWAY_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "planck/core/config.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/engine/shard.hpp"
#include "planck/gateway/credentials.hpp"
#include "planck/mem/arena.hpp"
#include "planck/net/poller.hpp"
#include "planck/net/socket.hpp"
#include "planck/perf/histogram.hpp"
#include "planck/wire/codec.hpp"

namespace planck::gateway {

// Which shard trades a symbol.
struct Route {
    core::SymbolId symbol{};
    // Index into GatewayConfig::shards.
    std::uint8_t shard_index = 0;
};

struct GatewayConfig {
    // Where clients connect. Port 0 lets the kernel choose: see
    // Gateway::local_endpoint().
    net::Endpoint listen{net::kAnyAddress, 9000};
    // Every shard, already initialised. The gateway becomes the only producer
    // on each shard's command ring and the only consumer of its report ring.
    std::span<engine::Shard* const> shards{};
    std::span<const Route> routes{};

    // Most clients connected at once.
    std::uint32_t max_sessions = 256;
    // Outbound bytes a client may have queued before it is judged too slow
    // and disconnected.
    std::uint32_t tx_buffer_bytes = 64U << 10;

    // Send a Heartbeat to a logged-in client that has been sent nothing for
    // this long, so it can tell a quiet venue from a dead one. Zero: never.
    std::uint32_t heartbeat_interval_ms = 0;
    // End a connection that nothing has been heard on for this long. That
    // is how a line that died without closing is noticed, and its session's
    // orders cancelled. A connection also has this long to log in, whatever
    // it sends meanwhile. Zero: never. Otherwise heartbeats must be on and
    // this must be longer than their interval, and at most kMaxIdleTimeoutMs
    // because the client is told it in sixteen bits.
    std::uint32_t idle_timeout_ms = 0;
    // Where the gateway reads the time, in nanoseconds that never go
    // backwards. Null for the system's monotonic clock. Tests pass their own
    // so that they need not wait for real time to pass.
    std::uint64_t (*clock)() noexcept = nullptr;

    // Who may log in. If null, any client id from 1 to 65,535 may, and the
    // token is ignored: fine for tests and a closed lab, nowhere else. The
    // table must outlive the gateway and must not change while it runs.
    const Credentials* credentials = nullptr;

    // If set, the gateway records here how long each report took: from the
    // moment its request came off the socket to the moment the gateway
    // picked the report up to send. In ticks of the cycle counter. Only the
    // gateway thread may touch the histogram while the gateway runs.
    perf::Histogram* latency = nullptr;

    bool pin = false;
    std::size_t cpu = 0;
    engine::IdlePolicy idle = engine::IdlePolicy::Spin;
};

// Plain counters, written only by the gateway thread.
struct GatewayStats {
    std::uint64_t connections = 0;
    std::uint64_t logins = 0;
    std::uint64_t disconnects = 0;
    std::uint64_t messages_in = 0;
    std::uint64_t commands_out = 0;
    std::uint64_t reports_out = 0;
    // Requests refused here, before reaching a shard.
    std::uint64_t rejected = 0;
    // Subset of rejected: the shard's command ring was full.
    std::uint64_t busy = 0;
    // Logins refused because the client is not listed or its token is wrong.
    std::uint64_t bad_credentials = 0;
    // Times a shard was told that a session had lost its connection. One
    // for each shard for each logged-in connection that ended.
    std::uint64_t session_closes = 0;
    // Subset of session_closes that could not be sent at once, because the
    // shard's command ring was full, and went out on a later pass.
    std::uint64_t session_closes_delayed = 0;
    // Heartbeats sent to clients that had been sent nothing else, and
    // heartbeats received.
    std::uint64_t heartbeats_out = 0;
    std::uint64_t heartbeats_in = 0;
    // Connections ended because nothing was heard on them for too long.
    std::uint64_t idle_timeouts = 0;
    // Connections ended because they did not log in soon enough.
    std::uint64_t login_timeouts = 0;
    // Reports for a session that had no connection at the time.
    std::uint64_t reports_dropped = 0;
    // Connections dropped for sending bytes that are not the protocol.
    std::uint64_t protocol_errors = 0;
    // Connections dropped for not reading their reports fast enough.
    std::uint64_t slow_consumers = 0;
};

// The order entry front door. One thread that owns every client connection.
//
//   client --TCP--> [ gateway ] --commands--> shard
//   client <--TCP-- [ gateway ] <--reports--- shard
//
// Inbound, it frames the byte stream, applies the trust boundary, and turns
// each valid request into a Command on the right shard's ring. Outbound, it
// drains each shard's reports and copies the already-encoded bytes to the
// connection of the session they belong to. It never touches a book.
//
// A session and a connection are different things. A session is a client's
// standing with the venue: it owns orders, and its id is the client id the
// client logged in with, the same on every connection and across restarts.
// A connection is one TCP link, here today and gone tomorrow. At most one
// connection speaks for a session at a time.
//
// When a connection that had logged in ends, for whatever reason, the
// gateway tells every shard that the session has closed, and a shard that
// is configured to cancel on disconnect then cancels what the session had
// resting. When the gateway itself starts it tells every shard that all
// sessions are closed, because none of them has a connection yet. Like
// every other command these are never dropped: one that does not fit in a
// shard's ring is owed, and sent on a later pass. While a session is owed a
// close it cannot log in again, or the late close would cancel the orders
// of its new connection.
//
// A client that reconnects is the same owner as before. Reports produced
// while it had no connection are dropped and counted.
//
// A line can die without the kernel ever saying so. So, if configured to,
// the gateway ends any connection it has heard nothing on for too long,
// and sends a Heartbeat to any client it has had nothing else to say to.
// It judges silence by its own clock, and only while it is itself running:
// after a pause of its own it gives everyone a fresh start, since whatever
// they sent meanwhile is still waiting to be read.
//
// Lifecycle matches Shard: construct anywhere, init() on the thread that
// will run it, then run() or poll().
class Gateway {
public:
    static constexpr std::size_t kRxBufferBytes = 4096;
    static constexpr std::uint32_t kMaxSessionId = 0xFFFF;
    static constexpr std::uint32_t kMaxIdleTimeoutMs = 0xFFFF;
    // A gap this long between two passes means the gateway itself was held
    // up, and nobody is blamed for the silence.
    static constexpr std::uint64_t kOwnPauseNs = 100'000'000;

    Gateway() noexcept = default;
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    [[nodiscard]] core::Status init(const GatewayConfig& config) noexcept;

    // One pass: accept, read, route, write. Never blocks. Returns how many
    // things it did, so a caller can tell busy from idle.
    std::size_t poll() noexcept;

    void run(const std::atomic<bool>& stop) noexcept;

    [[nodiscard]] bool initialised() const noexcept { return listener_.valid(); }
    [[nodiscard]] core::Status local_endpoint(net::Endpoint& out) const noexcept {
        return listener_.local_endpoint(out);
    }
    // Clients connected right now, logged in or not.
    [[nodiscard]] std::uint32_t active_sessions() const noexcept { return active_; }
    [[nodiscard]] const GatewayStats& stats() const noexcept { return stats_; }

private:
    struct Connection {
        net::Socket socket;
        // Zero until the client has logged in. Then its client id.
        core::SessionId session{};
        bool open = false;     // false while the slot is free
        bool closing = false;  // goes as soon as what is queued has been sent
        bool dirty = false;    // has bytes waiting in tx
        std::uint32_t rx_length = 0;
        std::uint32_t tx_length = 0;
        // When it connected, when bytes last arrived on it, and when
        // something was last queued for it. On the gateway's clock.
        std::uint64_t opened_ns = 0;
        std::uint64_t heard_ns = 0;
        std::uint64_t sent_ns = 0;
        std::byte* rx = nullptr;
        std::byte* tx = nullptr;
    };

    static constexpr std::size_t kSlots = core::kMaxSessions;
    static constexpr std::uint64_t kListenerToken = ~std::uint64_t{0};
    static constexpr std::uint8_t kNoShard = 0xFF;
    static constexpr std::size_t kMaxAcceptsPerPoll = 8;
    static constexpr std::size_t kMaxReadsPerEvent = 4;
    static constexpr std::size_t kMaxReportsPerShard = 256;

    static constexpr std::size_t kSessionIds = std::size_t{kMaxSessionId} + 1;
    static constexpr std::uint16_t kNoConnection = 0xFFFF;
    static_assert(kSlots <= kNoConnection);

    [[nodiscard]] std::uint16_t slot_of(const Connection& connection) const noexcept {
        return static_cast<std::uint16_t>(&connection - connections_.data());
    }

    // The connection that speaks for this session now, or nullptr.
    [[nodiscard]] Connection* find(core::SessionId session) noexcept {
        const std::uint16_t slot = slot_by_session_[session.raw()];
        return (slot == kNoConnection) ? nullptr : &connections_[slot];
    }

    // Read the clock, and every so often look for connections that have
    // gone silent and clients that are due a heartbeat.
    void keep_time() noexcept;
    void check_connections() noexcept;

    // Tell every shard that a session has lost its connection.
    void close_session(core::SessionId session) noexcept;
    // Send the closes that did not fit earlier.
    void send_owed_closes() noexcept;

    // A free slot, or nullptr if every one is taken.
    [[nodiscard]] Connection* free_slot() noexcept;

    std::size_t accept_clients() noexcept;
    std::size_t read_connection(Connection& connection) noexcept;
    std::size_t route_reports() noexcept;
    void flush_dirty() noexcept;
    void disconnect(Connection& connection) noexcept;

    // Handle every complete message in the connection's receive buffer. False
    // if the connection was closed.
    [[nodiscard]] bool consume(Connection& connection) noexcept;
    // False if the message is a protocol violation and the connection must go.
    [[nodiscard]] bool dispatch(Connection& connection, const wire::Frame& frame,
                                std::uint64_t ingress_tsc) noexcept;
    void on_login(Connection& connection, const wire::LoginRequest& request) noexcept;
    void on_new_order(Connection& connection, const wire::NewOrder& order,
                      std::uint64_t ingress_tsc) noexcept;
    void on_cancel(Connection& connection, const wire::CancelOrder& cancel,
                   std::uint64_t ingress_tsc) noexcept;
    void reject_order(Connection& connection, std::uint64_t client_order_id,
                      wire::RejectCode reason) noexcept;
    void reject_cancel(Connection& connection, std::uint64_t order_id, wire::RejectCode reason) noexcept;

    // Queue bytes for a connection. Disconnects it if its buffer is full.
    void queue(Connection& connection, std::span<const std::byte> bytes) noexcept;
    template <wire::Message T>
    void queue_message(Connection& connection, const T& message) noexcept;

    void idle() const noexcept;

    net::Socket listener_{};
    net::Poller poller_{};
    std::span<engine::Shard* const> shards_{};
    const Credentials* credentials_ = nullptr;
    std::uint64_t (*clock_)() noexcept = nullptr;
    // Zero means off, for both.
    std::uint64_t heartbeat_ns_ = 0;
    std::uint64_t idle_timeout_ns_ = 0;
    std::uint16_t idle_timeout_ms_ = 0;
    // The time as of this pass, and when connections are next looked over.
    std::uint64_t now_ns_ = 0;
    std::uint64_t next_check_ns_ = 0;
    std::uint64_t check_every_ns_ = 0;
    std::size_t next_slot_ = 0;
    perf::Histogram* latency_ = nullptr;
    std::uint32_t active_ = 0;
    std::uint32_t max_sessions_ = 0;
    std::uint32_t tx_capacity_ = 0;
    std::size_t dirty_count_ = 0;
    engine::IdlePolicy idle_ = engine::IdlePolicy::Spin;
    GatewayStats stats_{};

    // Shard index by symbol id, and by the shard id carried in an order id.
    std::array<std::uint8_t, core::kMaxSymbols> shard_by_symbol_{};
    std::array<std::uint8_t, core::kMaxShards> shard_by_id_{};
    std::array<std::uint16_t, kSlots> dirty_slots_{};
    std::array<Connection, kSlots> connections_{};
    // Slot of the connection speaking for each session id, or kNoConnection.
    std::array<std::uint16_t, kSessionIds> slot_by_session_{};
    // For each session id, the shards still to be told that it has closed:
    // one bit per index into shards_. And the sessions that have any.
    std::array<std::uint16_t, kSessionIds> close_owed_{};
    std::array<std::uint16_t, kSessionIds> owed_sessions_{};
    std::size_t owed_count_ = 0;
    static_assert(core::kMaxShards <= 16);
    mem::Arena arena_{};
};

}  // namespace planck::gateway

#endif  // PLANCK_GATEWAY_GATEWAY_HPP
