#include "planck/feed/publisher.hpp"

#include <algorithm>
#include <cstring>

#include <sched.h>

#include "planck/core/bits.hpp"
#include "planck/core/contract.hpp"
#include "planck/core/hint.hpp"
#include "planck/sys/cpu.hpp"

namespace planck::feed {

namespace {

using core::ok;
using core::Status;

constexpr std::uint32_t kMinHistory = 1024;
constexpr std::uint32_t kMaxHistory = 1U << 24;

Status validate(const PublisherConfig& config) noexcept {
    if (config.shards.empty() || config.shards.size() > core::kMaxShards ||
        config.destinations.size() != config.shards.size()) {
        return Status::InvalidArgument;
    }
    if (!core::is_pow2(config.history_messages) || config.history_messages < kMinHistory ||
        config.history_messages > kMaxHistory) {
        return Status::InvalidArgument;
    }
    if (config.heartbeat_interval.raw() == 0) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

}  // namespace

Status Publisher::init(const PublisherConfig& config) noexcept {
    if (initialised()) {
        return Status::Duplicate;
    }
    if (const Status status = validate(config); !ok(status)) {
        return status;
    }
    if (config.pin) {
        sys::HotThreadReport report;
        const sys::HotThreadConfig thread{.cpu = config.cpu, .name = "planck-feed"};
        if (const Status status = sys::configure_hot_thread(thread, report); !ok(status)) {
            return status;
        }
    }

    const std::size_t count = config.shards.size();
    const std::size_t history_bytes =
        count * config.history_messages * sizeof(engine::MarketEvent) + (std::size_t{64} << 10);
    if (const Status status = arena_.init(history_bytes, sys::HugePages::Try); !ok(status)) {
        return status;
    }
    if (const Status status = clock_.calibrate(); !ok(status)) {
        return status;
    }

    for (std::size_t i = 0; i < count; ++i) {
        engine::Shard* const shard = config.shards[i];
        if (shard == nullptr || !shard->initialised()) {
            return Status::InvalidArgument;
        }
        Stream& stream = streams_[i];
        const auto history = arena_.allocate_array<engine::MarketEvent>(config.history_messages);
        if (history.empty()) {
            return Status::Capacity;
        }
        if (const Status status = net::udp_open_sender(config.destinations[i], stream.socket,
                                                       config.multicast_ttl);
            !ok(status)) {
            return status;
        }
        stream.shard = shard;
        stream.history = history.data();
        stream.next_sequence = 1;
        stream.last_send_tsc = sys::tsc_now();
        stream.stream_id = shard->id().raw();
    }
    arena_.prefault();

    if (const Status status = net::udp_open_receiver(config.retransmit_listen, requests_);
        !ok(status)) {
        return status;
    }

    history_mask_ = config.history_messages - 1;
    // Heartbeat interval in counter ticks, so the hot loop compares two
    // counter readings and never converts to nanoseconds.
    heartbeat_ticks_ = static_cast<std::uint64_t>(
        (static_cast<sys::detail::U128>(config.heartbeat_interval.raw()) * clock_.ticks_per_second()) /
        1'000'000'000U);
    idle_ = config.idle;
    stream_count_ = count;
    return Status::Ok;
}

std::size_t Publisher::poll() noexcept {
    PLANCK_EXPECT(initialised());
    std::size_t work = 0;
    for (std::size_t i = 0; i < stream_count_; ++i) {
        Stream& stream = streams_[i];
        const std::size_t published = publish(stream);
        if (published == 0) {
            heartbeat_if_quiet(stream);
        }
        work += published;
    }
    work += serve_requests();
    return work;
}

void Publisher::run(const std::atomic<bool>& stop) noexcept {
    PLANCK_EXPECT(initialised());
    while (!stop.load(std::memory_order_relaxed)) {
        if (poll() == 0) {
            idle();
        }
    }
}

std::size_t Publisher::publish(Stream& stream) noexcept {
    PLANCK_EXPECT(packet_.count == 0);
    const std::size_t taken = stream.shard->market().drain(
        kMaxEventsPerPoll, [this, &stream](const engine::MarketEvent& event) noexcept {
            // Keep it for retransmission first, then put it on the wire.
            stream.history[stream.next_sequence & history_mask_] = event;
            append(stream, event);
            ++stream.next_sequence;
        });
    if (packet_.count != 0) {
        flush(stream, nullptr);
    }
    stats_.messages += taken;
    return taken;
}

void Publisher::append(Stream& stream, const engine::MarketEvent& event) noexcept {
    PLANCK_EXPECT(event.length >= sizeof(wire::MsgHeader) &&
                  event.length <= engine::kMaxMarketMessageSize);

    // A message never straddles two datagrams. Send what is there and start
    // a fresh one.
    if (packet_.length + event.length > packet_.bytes.size()) {
        flush(stream, nullptr);
    }
    if (packet_.count == 0) {
        packet_.first_sequence = stream.next_sequence;
    }
    std::memcpy(packet_.bytes.data() + packet_.length, event.bytes.data(), event.length);
    packet_.length += event.length;
    ++packet_.count;
}

void Publisher::flush(Stream& stream, const net::Endpoint* to) noexcept {
    const wire::PacketHeader header{.sequence = packet_.first_sequence,
                                    .count = packet_.count,
                                    .stream_id = stream.stream_id,
                                    .reserved = 0};
    std::memcpy(packet_.bytes.data(), &header, sizeof(header));

    const std::span<const std::byte> datagram{packet_.bytes.data(), packet_.length};
    const net::IoResult result =
        (to == nullptr) ? stream.socket.send(datagram) : requests_.send_to(datagram, *to);
    if (ok(result.status)) {
        ++stats_.datagrams;
    } else {
        // UDP makes no promises and neither does this. The sequence numbers
        // let subscribers notice and ask again.
        ++stats_.send_failures;
    }
    if (to == nullptr) {
        stream.last_send_tsc = sys::tsc_now();
    }
    packet_.length = sizeof(wire::PacketHeader);
    packet_.count = 0;
}

void Publisher::heartbeat_if_quiet(Stream& stream) noexcept {
    if ((sys::tsc_now() - stream.last_send_tsc) < heartbeat_ticks_) {
        return;
    }
    // An empty datagram whose sequence is the next one to be used. A
    // subscriber that expected a lower number knows it has missed something.
    packet_.first_sequence = stream.next_sequence;
    PLANCK_ASSERT(packet_.count == 0);
    flush(stream, nullptr);
    ++stats_.heartbeats;
}

std::size_t Publisher::serve_requests() noexcept {
    std::size_t served = 0;
    for (std::size_t i = 0; i < kMaxRequestsPerPoll; ++i) {
        std::array<std::byte, 64> buffer{};
        net::Endpoint from;
        const net::IoResult result = requests_.receive_from(buffer, from);
        if (!ok(result.status)) {
            break;
        }
        ++served;
        ++stats_.retransmit_requests;

        // Anyone can send anything to this port. Check all of it.
        wire::PacketHeader request;
        if (result.bytes != sizeof(request)) {
            ++stats_.retransmit_refused;
            continue;
        }
        std::memcpy(&request, buffer.data(), sizeof(request));
        Stream* stream = nullptr;
        for (std::size_t s = 0; s < stream_count_; ++s) {
            if (streams_[s].stream_id == request.stream_id) {
                stream = &streams_[s];
            }
        }
        if (stream == nullptr || request.count == 0 || request.reserved != 0) {
            ++stats_.retransmit_refused;
            continue;
        }
        retransmit(*stream, request.sequence, request.count, from);
    }
    return served;
}

void Publisher::retransmit(Stream& stream, std::uint64_t first, std::uint32_t count,
                           const net::Endpoint& to) noexcept {
    PLANCK_EXPECT(count != 0);
    PLANCK_EXPECT(packet_.count == 0);

    // What the history still holds: the last (mask + 1) messages published.
    const std::uint64_t end = stream.next_sequence;
    const std::uint64_t capacity = history_mask_ + 1;
    const std::uint64_t oldest = (end > capacity) ? (end - capacity) : 1;
    if (first < oldest || first >= end) {
        ++stats_.retransmit_refused;
        return;
    }
    const std::uint64_t wanted = std::min<std::uint64_t>(count, kMaxRetransmitMessages);
    const std::uint64_t last = std::min(first + wanted, end);

    for (std::uint64_t sequence = first; sequence < last; ++sequence) {
        const engine::MarketEvent& event = stream.history[sequence & history_mask_];
        if (packet_.length + event.length > packet_.bytes.size()) {
            flush(stream, &to);
        }
        if (packet_.count == 0) {
            packet_.first_sequence = sequence;
        }
        std::memcpy(packet_.bytes.data() + packet_.length, event.bytes.data(), event.length);
        packet_.length += event.length;
        ++packet_.count;
        ++stats_.retransmitted_messages;
    }
    if (packet_.count != 0) {
        flush(stream, &to);
    }
}

void Publisher::idle() const noexcept {
    if (idle_ == engine::IdlePolicy::Spin) {
        core::cpu_relax();
    } else {
        static_cast<void>(::sched_yield());
    }
}

}  // namespace planck::feed
