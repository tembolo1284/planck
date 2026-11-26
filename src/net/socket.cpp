#include "planck/net/socket.hpp"

#include <cerrno>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "planck/core/contract.hpp"
#include "planck/sys/error.hpp"

namespace planck::net {

namespace {

using core::ok;
using core::Status;

// A signal can interrupt a socket call before it does anything. Retry a few
// times, then report WouldBlock and let the caller come round again.
constexpr int kMaxInterruptRetries = 4;

sockaddr_in to_sockaddr(Endpoint endpoint) noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    address.sin_addr.s_addr = htonl(endpoint.address);
    return address;
}

Endpoint from_sockaddr(const sockaddr_in& address) noexcept {
    return Endpoint{.address = ntohl(address.sin_addr.s_addr), .port = ntohs(address.sin_port)};
}

const sockaddr* as_sockaddr(const sockaddr_in& address) noexcept {
    return reinterpret_cast<const sockaddr*>(&address);
}

Status set_option(int fd, int level, int name, int value) noexcept {
    if (::setsockopt(fd, level, name, &value, sizeof(value)) != 0) {
        return sys::status_from_errno(errno);
    }
    return Status::Ok;
}

Status open_socket(int type, bool non_blocking, Socket& out) noexcept {
    const int flags = type | SOCK_CLOEXEC | (non_blocking ? SOCK_NONBLOCK : 0);
    const int fd = ::socket(AF_INET, flags, 0);
    if (fd < 0) {
        return sys::status_from_errno(errno);
    }
    out = Socket::adopt(fd, type == SOCK_STREAM);
    return Status::Ok;
}

Status make_non_blocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return sys::status_from_errno(errno);
    }
    return Status::Ok;
}

// Translate the result of a read or write system call.
IoResult io_result(ssize_t transferred, bool end_of_stream_possible) noexcept {
    if (transferred > 0) {
        return IoResult{.status = Status::Ok, .bytes = static_cast<std::size_t>(transferred)};
    }
    if (transferred == 0) {
        return IoResult{.status = end_of_stream_possible ? Status::Closed : Status::Ok, .bytes = 0};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return IoResult{.status = Status::WouldBlock, .bytes = 0};
    }
    return IoResult{.status = sys::status_from_errno(errno), .bytes = 0};
}

}  // namespace

void Socket::close() noexcept {
    if (fd_ >= 0) {
        // Nothing useful can be done if close fails, and the descriptor is
        // released either way.
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

IoResult Socket::receive(std::span<std::byte> buffer) noexcept {
    PLANCK_EXPECT(valid());
    PLANCK_EXPECT(!buffer.empty());
    ssize_t got = -1;
    for (int attempt = 0; attempt < kMaxInterruptRetries; ++attempt) {
        got = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (got >= 0 || errno != EINTR) {
            break;
        }
    }
    return io_result(got, stream_);
}

IoResult Socket::send(std::span<const std::byte> buffer) noexcept {
    PLANCK_EXPECT(valid());
    PLANCK_EXPECT(!buffer.empty());
    ssize_t sent = -1;
    for (int attempt = 0; attempt < kMaxInterruptRetries; ++attempt) {
        // MSG_NOSIGNAL: a dead peer must come back as an error, not SIGPIPE.
        sent = ::send(fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL);
        if (sent >= 0 || errno != EINTR) {
            break;
        }
    }
    return io_result(sent, false);
}

IoResult Socket::receive_from(std::span<std::byte> buffer, Endpoint& from) noexcept {
    PLANCK_EXPECT(valid() && !stream_);
    PLANCK_EXPECT(!buffer.empty());
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    ssize_t got = -1;
    for (int attempt = 0; attempt < kMaxInterruptRetries; ++attempt) {
        length = sizeof(address);
        got = ::recvfrom(fd_, buffer.data(), buffer.size(), 0,
                         reinterpret_cast<sockaddr*>(&address), &length);
        if (got >= 0 || errno != EINTR) {
            break;
        }
    }
    if (got >= 0) {
        from = from_sockaddr(address);
    }
    return io_result(got, false);
}

IoResult Socket::send_to(std::span<const std::byte> buffer, Endpoint to) noexcept {
    PLANCK_EXPECT(valid() && !stream_);
    PLANCK_EXPECT(!buffer.empty());
    const sockaddr_in address = to_sockaddr(to);
    ssize_t sent = -1;
    for (int attempt = 0; attempt < kMaxInterruptRetries; ++attempt) {
        sent = ::sendto(fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL, as_sockaddr(address),
                        sizeof(address));
        if (sent >= 0 || errno != EINTR) {
            break;
        }
    }
    return io_result(sent, false);
}

Status Socket::local_endpoint(Endpoint& out) const noexcept {
    PLANCK_EXPECT(valid());
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return sys::status_from_errno(errno);
    }
    out = from_sockaddr(address);
    return Status::Ok;
}

Status tcp_listen(Endpoint local, Socket& out, int backlog) noexcept {
    Socket socket;
    if (const Status status = open_socket(SOCK_STREAM, true, socket); !ok(status)) {
        return status;
    }
    if (const Status status = set_option(socket.fd(), SOL_SOCKET, SO_REUSEADDR, 1); !ok(status)) {
        return status;
    }
    const sockaddr_in address = to_sockaddr(local);
    if (::bind(socket.fd(), as_sockaddr(address), sizeof(address)) != 0) {
        return sys::status_from_errno(errno);
    }
    if (::listen(socket.fd(), backlog) != 0) {
        return sys::status_from_errno(errno);
    }
    out = std::move(socket);
    return Status::Ok;
}

Status tcp_accept(const Socket& listener, Socket& out, Endpoint& peer) noexcept {
    PLANCK_EXPECT(listener.valid());
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    const int fd = ::accept4(listener.fd(), reinterpret_cast<sockaddr*>(&address), &length,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        // A connection that died while still queued is not our problem.
        const bool nothing_usable = (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
                                     errno == ECONNABORTED);
        return nothing_usable ? Status::WouldBlock : sys::status_from_errno(errno);
    }
    Socket socket = Socket::adopt(fd, true);
    if (const Status status = set_option(fd, IPPROTO_TCP, TCP_NODELAY, 1); !ok(status)) {
        return status;
    }
    peer = from_sockaddr(address);
    out = std::move(socket);
    return Status::Ok;
}

Status tcp_connect(Endpoint remote, Socket& out) noexcept {
    Socket socket;
    if (const Status status = open_socket(SOCK_STREAM, false, socket); !ok(status)) {
        return status;
    }
    const sockaddr_in address = to_sockaddr(remote);
    int rc = -1;
    for (int attempt = 0; attempt < kMaxInterruptRetries; ++attempt) {
        rc = ::connect(socket.fd(), as_sockaddr(address), sizeof(address));
        if (rc == 0 || errno != EINTR) {
            break;
        }
    }
    if (rc != 0) {
        return (errno == ECONNREFUSED) ? Status::Closed : sys::status_from_errno(errno);
    }
    if (const Status status = set_option(socket.fd(), IPPROTO_TCP, TCP_NODELAY, 1); !ok(status)) {
        return status;
    }
    if (const Status status = make_non_blocking(socket.fd()); !ok(status)) {
        return status;
    }
    out = std::move(socket);
    return Status::Ok;
}

Status udp_open_sender(Endpoint destination, Socket& out, std::uint8_t ttl, bool loopback) noexcept {
    Socket socket;
    if (const Status status = open_socket(SOCK_DGRAM, true, socket); !ok(status)) {
        return status;
    }
    if (is_multicast(destination.address)) {
        if (const Status status = set_option(socket.fd(), IPPROTO_IP, IP_MULTICAST_TTL, ttl);
            !ok(status)) {
            return status;
        }
        if (const Status status =
                set_option(socket.fd(), IPPROTO_IP, IP_MULTICAST_LOOP, loopback ? 1 : 0);
            !ok(status)) {
            return status;
        }
    }
    // Connecting a UDP socket fixes its destination, so the hot path can use
    // plain send() and the kernel skips a route lookup per datagram.
    const sockaddr_in address = to_sockaddr(destination);
    if (::connect(socket.fd(), as_sockaddr(address), sizeof(address)) != 0) {
        return sys::status_from_errno(errno);
    }
    out = std::move(socket);
    return Status::Ok;
}

Status udp_open_receiver(Endpoint local, Socket& out) noexcept {
    Socket socket;
    if (const Status status = open_socket(SOCK_DGRAM, true, socket); !ok(status)) {
        return status;
    }
    if (const Status status = set_option(socket.fd(), SOL_SOCKET, SO_REUSEADDR, 1); !ok(status)) {
        return status;
    }

    const bool multicast = is_multicast(local.address);
    // A multicast receiver binds to the group address itself, so it does not
    // also pick up unrelated traffic sent to the same port.
    const sockaddr_in address = to_sockaddr(local);
    if (::bind(socket.fd(), as_sockaddr(address), sizeof(address)) != 0) {
        return sys::status_from_errno(errno);
    }
    if (multicast) {
        ip_mreq request{};
        request.imr_multiaddr.s_addr = htonl(local.address);
        request.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(socket.fd(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) != 0) {
            return sys::status_from_errno(errno);
        }
    }
    out = std::move(socket);
    return Status::Ok;
}

}  // namespace planck::net
