#include "planck/net/poller.hpp"

#include <algorithm>
#include <array>
#include <cerrno>

#include <sys/epoll.h>
#include <unistd.h>

#include "planck/core/contract.hpp"
#include "planck/sys/error.hpp"

namespace planck::net {

using core::Status;

Status Poller::init() noexcept {
    if (initialised()) {
        return Status::Duplicate;
    }
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        return sys::status_from_errno(errno);
    }
    fd_ = fd;
    return Status::Ok;
}

void Poller::close() noexcept {
    if (fd_ >= 0) {
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

Status Poller::add(const Socket& socket, std::uint64_t token) noexcept {
    PLANCK_EXPECT(initialised());
    PLANCK_EXPECT(socket.valid());
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.u64 = token;
    if (::epoll_ctl(fd_, EPOLL_CTL_ADD, socket.fd(), &event) != 0) {
        return sys::status_from_errno(errno);
    }
    return Status::Ok;
}

Status Poller::remove(const Socket& socket) noexcept {
    PLANCK_EXPECT(initialised());
    PLANCK_EXPECT(socket.valid());
    if (::epoll_ctl(fd_, EPOLL_CTL_DEL, socket.fd(), nullptr) != 0) {
        return sys::status_from_errno(errno);
    }
    return Status::Ok;
}

std::size_t Poller::wait(std::span<Event> out, int timeout_ms) noexcept {
    PLANCK_EXPECT(initialised());
    PLANCK_EXPECT(!out.empty());

    std::array<epoll_event, kMaxEvents> ready{};
    const int capacity = static_cast<int>(std::min(out.size(), ready.size()));
    const int count = ::epoll_wait(fd_, ready.data(), capacity, timeout_ms);
    if (count <= 0) {
        // Zero is a timeout. Negative is an interrupted call, which the
        // caller's next wait() will make up for.
        return 0;
    }

    const auto found = static_cast<std::size_t>(count);
    for (std::size_t i = 0; i < found; ++i) {
        const std::uint32_t flags = ready[i].events;
        out[i] = Event{.token = ready[i].data.u64,
                       .readable = (flags & EPOLLIN) != 0,
                       .closed = (flags & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0};
    }
    return found;
}

}  // namespace planck::net
