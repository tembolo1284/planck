#ifndef PLANCK_FEED_RECEIVER_HPP
#define PLANCK_FEED_RECEIVER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "planck/core/contract.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/net/socket.hpp"
#include "planck/wire/codec.hpp"
#include "planck/wire/market_data.hpp"

namespace planck::feed {

struct ReceiverConfig {
    // Where the stream arrives: a multicast group and port, or a plain local
    // address and port. Port 0 lets the kernel choose.
    net::Endpoint listen{};
    // Which shard's stream to accept. Datagrams for any other are ignored.
    std::uint16_t stream_id = 0;
    // The publisher's retransmit port. Port 0 means there is none, and any
    // gap is reported as lost straight away.
    net::Endpoint retransmit_server{};
    // True: begin at sequence 1 and ask the publisher for everything missed
    // so far. False: begin at whatever arrives first.
    bool from_start = false;
};

struct ReceiverStats {
    std::uint64_t datagrams = 0;
    std::uint64_t messages = 0;
    // Datagrams or messages already seen, or not for this stream.
    std::uint64_t ignored = 0;
    std::uint64_t gaps = 0;
    std::uint64_t requests = 0;
    // Messages given up on. Nonzero means the subscriber's books are wrong.
    std::uint64_t lost = 0;
};

// The subscriber's side of the feed. Takes datagrams as they arrive, in
// whatever order and with whatever missing, and hands the messages inside to
// a handler exactly once each and strictly in sequence.
//
// On a gap it stops delivering, asks the publisher to resend from the first
// missing message, and discards anything newer until the hole is filled.
// That is simple and always correct, at the cost of re-requesting messages it
// had already received. If the publisher never answers, the gap is declared
// lost and delivery resumes from the live stream. A subscriber that sees
// stats().lost go up no longer has complete books.
class Receiver {
public:
    // How long to wait for a retransmission before asking again, and how
    // many times to ask before giving up.
    static constexpr core::Nanos kRequestInterval{2'000'000};
    static constexpr std::uint32_t kMaxRequests = 100;

    Receiver() noexcept = default;
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    [[nodiscard]] core::Status init(const ReceiverConfig& config) noexcept;

    // Read what has arrived and deliver it. handler is called as
    //     handler(std::uint64_t sequence, std::span<const std::byte> message)
    // where message is one complete wire message. Never blocks. Returns how
    // many messages were delivered.
    template <typename H>
        requires std::is_nothrow_invocable_v<H&, std::uint64_t, std::span<const std::byte>>
    std::size_t poll(H&& handler) noexcept {
        PLANCK_EXPECT(initialised());
        std::size_t delivered = 0;
        std::array<std::byte, wire::kMaxDatagramSize> buffer{};
        for (net::Socket* const socket : {&live_, &replies_}) {
            for (std::size_t i = 0; i < kMaxDatagramsPerPoll && socket->valid(); ++i) {
                net::Endpoint from;
                const net::IoResult result = socket->receive_from(buffer, from);
                if (!core::ok(result.status)) {
                    break;
                }
                delivered += ingest({buffer.data(), result.bytes}, handler);
            }
        }
        chase_gap();
        return delivered;
    }

    // Process one datagram. poll() calls this for each one it reads. It is
    // public so a test, or a capture replay, can feed datagrams by hand.
    template <typename H>
        requires std::is_nothrow_invocable_v<H&, std::uint64_t, std::span<const std::byte>>
    std::size_t ingest(std::span<const std::byte> datagram, H&& handler) noexcept {
        wire::PacketHeader header;
        if (!accept(datagram, header)) {
            return 0;
        }

        std::size_t delivered = 0;
        std::size_t offset = sizeof(header);
        for (std::uint16_t i = 0; i < header.count; ++i) {
            const wire::Frame frame = wire::next_frame(datagram.subspan(offset));
            if (!core::ok(frame.status)) {
                // Damaged past this point. The rest will show up as a gap.
                break;
            }
            offset += frame.bytes.size();
            const std::uint64_t sequence = header.sequence + i;
            if (sequence < expected_) {
                ++stats_.ignored;
                continue;
            }
            PLANCK_ASSERT(sequence == expected_);
            handler(sequence, frame.bytes);
            ++expected_;
            ++delivered;
        }
        stats_.messages += delivered;
        if (delivered != 0) {
            made_progress();
        }
        return delivered;
    }

    [[nodiscard]] bool initialised() const noexcept { return live_.valid(); }
    // Sequence number of the next message to be delivered. Zero until the
    // first datagram arrives.
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return expected_; }
    // True while a hole is waiting to be filled.
    [[nodiscard]] bool recovering() const noexcept { return gap_end_ != 0; }
    [[nodiscard]] core::Status local_endpoint(net::Endpoint& out) const noexcept {
        return live_.local_endpoint(out);
    }
    [[nodiscard]] const ReceiverStats& stats() const noexcept { return stats_; }

private:
    static constexpr std::size_t kMaxDatagramsPerPoll = 64;

    // Decide whether a datagram's messages should be walked at all, and note
    // any gap it reveals. Fills header on true.
    [[nodiscard]] bool accept(std::span<const std::byte> datagram,
                              wire::PacketHeader& header) noexcept;
    void made_progress() noexcept;
    void chase_gap() noexcept;
    void send_request() noexcept;

    net::Socket live_{};
    net::Socket replies_{};
    net::Endpoint server_{};
    std::uint64_t expected_ = 0;
    // One past the newest sequence known to exist while recovering. Zero
    // when there is no gap.
    std::uint64_t gap_end_ = 0;
    std::uint64_t last_request_ns_ = 0;
    std::uint32_t requests_unanswered_ = 0;
    std::uint16_t stream_id_ = 0;
    ReceiverStats stats_{};
};

}  // namespace planck::feed

#endif  // PLANCK_FEED_RECEIVER_HPP
