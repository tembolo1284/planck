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
// So a client that reconnects is the same owner as before: it can cancel
// the orders it left and it receives their fills from then on. Reports
// produced while it had no connection are dropped and counted. Orders stay
// in the book while their owner is away.
//
// Lifecycle matches Shard: construct anywhere, init() on the thread that
// will run it, then run() or poll().
class Gateway {
public:
    static constexpr std::size_t kRxBufferBytes = 4096;
    static constexpr std::uint32_t kMaxSessionId = 0xFFFF;

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
    mem::Arena arena_{};
};

}  // namespace planck::gateway

#endif  // PLANCK_GATEWAY_GATEWAY_HPP
