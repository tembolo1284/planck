#include "planck/feed/receiver.hpp"

#include <algorithm>
#include <utility>

#include "planck/sys/clock.hpp"

namespace planck::feed {

using core::ok;
using core::Status;

Status Receiver::init(const ReceiverConfig& config) noexcept {
    if (initialised()) {
        return Status::Duplicate;
    }
    net::Socket live;
    if (const Status status = net::udp_open_receiver(config.listen, live); !ok(status)) {
        return status;
    }
    if (config.retransmit_server.port != 0) {
        // Requests go out of this socket and the replies come back to it.
        if (const Status status =
                net::udp_open_receiver(net::Endpoint{net::kAnyAddress, 0}, replies_);
            !ok(status)) {
            return status;
        }
    }
    server_ = config.retransmit_server;
    stream_id_ = config.stream_id;
    expected_ = config.from_start ? 1 : 0;
    gap_end_ = 0;
    live_ = std::move(live);
    return Status::Ok;
}

bool Receiver::accept(std::span<const std::byte> datagram, wire::PacketHeader& header) noexcept {
    if (datagram.size() < sizeof(header)) {
        ++stats_.ignored;
        return false;
    }
    std::memcpy(&header, datagram.data(), sizeof(header));
    if (header.stream_id != stream_id_ || header.reserved != 0 || header.sequence == 0) {
        ++stats_.ignored;
        return false;
    }
    ++stats_.datagrams;

    if (expected_ == 0) {
        // First contact. Start wherever the stream is now.
        expected_ = header.sequence;
    }
    // One past the last sequence this datagram proves to exist. A heartbeat
    // has no messages and its sequence is the next one to be used.
    const std::uint64_t proven_end = header.sequence + header.count;

    if (header.sequence > expected_) {
        // Something before this never arrived.
        if (gap_end_ == 0) {
            ++stats_.gaps;
            requests_unanswered_ = 0;
            last_request_ns_ = 0;
        }
        gap_end_ = std::max(gap_end_, proven_end);
        return false;
    }
    if (proven_end <= expected_) {
        if (header.count != 0) {
            ++stats_.ignored;  // nothing new in it
        }
        return false;
    }
    return true;
}

void Receiver::made_progress() noexcept {
    requests_unanswered_ = 0;
    if (gap_end_ != 0 && expected_ >= gap_end_) {
        gap_end_ = 0;  // hole filled
    } else if (gap_end_ != 0) {
        // Part of the hole is filled. Ask for the next part without waiting
        // out the retry interval.
        last_request_ns_ = 0;
    }
}

void Receiver::chase_gap() noexcept {
    if (gap_end_ == 0) {
        return;
    }
    PLANCK_ASSERT(expected_ != 0 && expected_ < gap_end_);

    const bool can_ask = replies_.valid() && (requests_unanswered_ < kMaxRequests);
    if (!can_ask) {
        // No publisher to ask, or it has stopped answering. Skip the hole and
        // carry on from the live stream. The caller's books are now wrong.
        stats_.lost += gap_end_ - expected_;
        expected_ = gap_end_;
        gap_end_ = 0;
        requests_unanswered_ = 0;
        return;
    }

    const std::uint64_t now = sys::monotonic_now().raw();
    if (last_request_ns_ == 0 || (now - last_request_ns_) >= kRequestInterval.raw()) {
        send_request();
        last_request_ns_ = now;
        ++requests_unanswered_;
    }
}

void Receiver::send_request() noexcept {
    const std::uint64_t missing = gap_end_ - expected_;
    const wire::PacketHeader request{
        .sequence = expected_,
        .count = static_cast<std::uint16_t>(std::min<std::uint64_t>(missing, 0xFFFF)),
        .stream_id = stream_id_,
        .reserved = 0};
    std::array<std::byte, sizeof(request)> bytes{};
    std::memcpy(bytes.data(), &request, sizeof(request));
    // If this is lost too, the retry timer sends another.
    static_cast<void>(replies_.send_to(bytes, server_));
    ++stats_.requests;
}

}  // namespace planck::feed
