#ifndef PLANCK_NET_POLLER_HPP
#define PLANCK_NET_POLLER_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "planck/core/status.hpp"
#include "planck/net/socket.hpp"

namespace planck::net {

// What happened on one registered socket.
struct Event {
    // The value given to add() for this socket.
    std::uint64_t token = 0;
    bool readable = false;
    // The peer hung up or the socket is in error. Read whatever is left,
    // then drop the socket.
    bool closed = false;
};

// Tells a thread which of many sockets have data waiting, in one system call.
// A thin wrapper over epoll, level-triggered: a socket keeps being reported
// for as long as it has unread data, so nothing is lost if a caller reads
// only part of what is there.
//
// A latency-critical thread calls wait() with a timeout of zero in a tight
// loop and never sleeps in the kernel. Anything else passes a real timeout.
class Poller {
public:
    // Most events one wait() call can return.
    static constexpr std::size_t kMaxEvents = 64;

    Poller() noexcept = default;
    ~Poller() noexcept { close(); }
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    [[nodiscard]] core::Status init() noexcept;
    void close() noexcept;
    [[nodiscard]] bool initialised() const noexcept { return fd_ >= 0; }

    // Start watching socket for incoming data. token comes back in every
    // Event for it. Duplicate if the socket is already registered.
    [[nodiscard]] core::Status add(const Socket& socket, std::uint64_t token) noexcept;

    // Stop watching. Do this before closing the socket.
    [[nodiscard]] core::Status remove(const Socket& socket) noexcept;

    // Fill out with ready sockets and return how many. timeout_ms of 0
    // returns at once, and a negative value waits without limit.
    [[nodiscard]] std::size_t wait(std::span<Event> out, int timeout_ms) noexcept;

private:
    int fd_ = -1;
};

}  // namespace planck::net

#endif  // PLANCK_NET_POLLER_HPP
