#ifndef PLANCK_FEED_PUBLISHER_HPP
#define PLANCK_FEED_PUBLISHER_HPP

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
#include "planck/net/socket.hpp"
#include "planck/sys/clock.hpp"
#include "planck/wire/market_data.hpp"

namespace planck::feed {

struct PublisherConfig {
    // Every shard, already initialised. The publisher becomes the only
    // consumer of each shard's market ring.
    std::span<engine::Shard* const> shards{};
    // Where each shard's stream is sent, one entry per shard in the same
    // order. Normally a multicast group and port each.
    std::span<const net::Endpoint> destinations{};

    // Where subscribers send retransmit requests. Port 0 lets the kernel
    // choose: see Publisher::retransmit_endpoint().
    net::Endpoint retransmit_listen{net::kAnyAddress, 0};
    // Messages kept per stream for retransmission. Power of two.
    std::uint32_t history_messages = 1U << 16;

    // A quiet stream sends a heartbeat this often, so subscribers can tell
    // "nothing happened" from "I lost the feed".
    core::Nanos heartbeat_interval{1'000'000'000};
    std::uint8_t multicast_ttl = 1;

    bool pin = false;
    std::size_t cpu = 0;
    engine::IdlePolicy idle = engine::IdlePolicy::Spin;
};

// Plain counters, written only by the publisher thread.
struct PublisherStats {
    std::uint64_t messages = 0;
    std::uint64_t datagrams = 0;
    std::uint64_t heartbeats = 0;
    // Datagrams the kernel refused. Subscribers see a gap and re-request.
    std::uint64_t send_failures = 0;
    std::uint64_t retransmit_requests = 0;
    std::uint64_t retransmitted_messages = 0;
    // Requests for messages older than the history holds, or otherwise bad.
    std::uint64_t retransmit_refused = 0;
};

// Turns each shard's market events into a public, sequenced UDP stream.
//
//   shard --market ring--> [ publisher ] --UDP--> everyone
//
// Each shard has its own stream with its own sequence numbers, starting at 1.
// Events are packed several to a datagram, never split across two, and every
// datagram says which sequence number it starts at.
//
// UDP loses packets. The publisher keeps the most recent messages of each
// stream and resends any range a subscriber asks for. A request is a single
// PacketHeader naming the stream, the first sequence wanted and how many,
// sent to the retransmit port. The reply goes back to the sender of the
// request in the same datagram format as the live stream.
class Publisher {
public:
    // Most messages resent for one request. A subscriber that needs more
    // asks again.
    static constexpr std::uint32_t kMaxRetransmitMessages = 512;

    Publisher() noexcept = default;
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    [[nodiscard]] core::Status init(const PublisherConfig& config) noexcept;

    // One pass over every stream, then service retransmit requests. Never
    // blocks. Returns how many things it did.
    std::size_t poll() noexcept;

    void run(const std::atomic<bool>& stop) noexcept;

    [[nodiscard]] bool initialised() const noexcept { return stream_count_ != 0; }
    [[nodiscard]] core::Status retransmit_endpoint(net::Endpoint& out) const noexcept {
        return requests_.local_endpoint(out);
    }
    // Sequence number the next message on a stream will carry.
    [[nodiscard]] std::uint64_t next_sequence(std::size_t stream) const noexcept {
        return streams_[stream].next_sequence;
    }
    [[nodiscard]] const PublisherStats& stats() const noexcept { return stats_; }

private:
    struct Stream {
        engine::Shard* shard = nullptr;
        net::Socket socket{};
        engine::MarketEvent* history = nullptr;
        std::uint64_t next_sequence = 1;
        std::uint64_t last_send_tsc = 0;
        std::uint16_t stream_id = 0;
    };

    // One datagram being filled.
    struct Packet {
        std::array<std::byte, wire::kMaxDatagramSize> bytes{};
        std::size_t length = sizeof(wire::PacketHeader);
        std::uint64_t first_sequence = 0;
        std::uint16_t count = 0;
    };

    static constexpr std::size_t kMaxEventsPerPoll = 256;
    static constexpr std::size_t kMaxRequestsPerPoll = 8;

    std::size_t publish(Stream& stream) noexcept;
    void append(Stream& stream, const engine::MarketEvent& event) noexcept;
    // Write the header and send. to == nullptr sends on the live stream.
    void flush(Stream& stream, const net::Endpoint* to) noexcept;
    void heartbeat_if_quiet(Stream& stream) noexcept;
    std::size_t serve_requests() noexcept;
    void retransmit(Stream& stream, std::uint64_t first, std::uint32_t count,
                    const net::Endpoint& to) noexcept;
    void idle() const noexcept;

    std::array<Stream, core::kMaxShards> streams_{};
    std::size_t stream_count_ = 0;
    Packet packet_{};
    net::Socket requests_{};
    std::uint64_t history_mask_ = 0;
    std::uint64_t heartbeat_ticks_ = 0;
    engine::IdlePolicy idle_ = engine::IdlePolicy::Spin;
    PublisherStats stats_{};
    sys::TscClock clock_{};
    mem::Arena arena_{};
};

}  // namespace planck::feed

#endif  // PLANCK_FEED_PUBLISHER_HPP
