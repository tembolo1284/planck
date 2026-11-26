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
#include "planck/mem/arena.hpp"
#include "planck/net/poller.hpp"
#include "planck/net/socket.hpp"
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
    // Reports for a session that had already gone.
    std::uint64_t reports_dropped = 0;
    // Sessions dropped for sending bytes that are not the protocol.
    std::uint64_t protocol_errors = 0;
    // Sessions dropped for not reading their reports fast enough.
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
// socket of the session they belong to. It never touches a book.
//
// Session ids are never reused within a run. A client that reconnects gets a
// new id, so it can neither cancel nor receive fills for orders left behind
// by an earlier connection. Those orders stay in the book until they trade.
// The price is a hard limit of 65,535 connections per run.
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
    [[nodiscard]] std::uint32_t active_sessions() const noexcept { return active_; }
    [[nodiscard]] const GatewayStats& stats() const noexcept { return stats_; }

private:
    struct Session {
        net::Socket socket;
        // Zero while the slot is free.
        core::SessionId id{};
        std::uint32_t client_id = 0;
        bool logged_in = false;
        bool dirty = false;  // has bytes waiting in tx
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

    [[nodiscard]] static constexpr std::size_t slot_of(core::SessionId id) noexcept {
        return id.raw() & (kSlots - 1);
    }

    // The live session with this id, or nullptr if it has gone.
    [[nodiscard]] Session* find(core::SessionId id) noexcept {
        Session& session = sessions_[slot_of(id)];
        return (id.raw() != 0 && session.id == id) ? &session : nullptr;
    }

    std::size_t accept_clients() noexcept;
    std::size_t read_session(Session& session) noexcept;
    std::size_t route_reports() noexcept;
    void flush_dirty() noexcept;
    void disconnect(Session& session) noexcept;

    // Handle every complete message in the session's receive buffer. False
    // if the session was disconnected.
    [[nodiscard]] bool consume(Session& session) noexcept;
    // False if the message is a protocol violation and the session must go.
    [[nodiscard]] bool dispatch(Session& session, const wire::Frame& frame,
                                std::uint64_t ingress_tsc) noexcept;
    void on_login(Session& session, const wire::LoginRequest& request) noexcept;
    void on_new_order(Session& session, const wire::NewOrder& order,
                      std::uint64_t ingress_tsc) noexcept;
    void on_cancel(Session& session, const wire::CancelOrder& cancel,
                   std::uint64_t ingress_tsc) noexcept;
    void reject_order(Session& session, std::uint64_t client_order_id,
                      wire::RejectCode reason) noexcept;
    void reject_cancel(Session& session, std::uint64_t order_id, wire::RejectCode reason) noexcept;

    // Queue bytes for a session. Disconnects it if its buffer is full.
    void queue(Session& session, std::span<const std::byte> bytes) noexcept;
    template <wire::Message T>
    void queue_message(Session& session, const T& message) noexcept;

    void idle() const noexcept;

    net::Socket listener_{};
    net::Poller poller_{};
    std::span<engine::Shard* const> shards_{};
    std::uint32_t next_session_id_ = 1;
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
    std::array<Session, kSlots> sessions_{};
    mem::Arena arena_{};
};

}  // namespace planck::gateway

#endif  // PLANCK_GATEWAY_GATEWAY_HPP
