#ifndef PLANCK_NET_SOCKET_HPP
#define PLANCK_NET_SOCKET_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "planck/core/status.hpp"
#include "planck/net/address.hpp"

namespace planck::net {

// Outcome of one read or write.
//   Ok          bytes were transferred. For a send this may be fewer than asked.
//   WouldBlock  nothing could be done right now. Try again later.
//   Closed      the peer is gone. Stop using the socket.
//   anything else is a hard error.
struct IoResult {
    core::Status status = core::Status::WouldBlock;
    std::size_t bytes = 0;
};

// Owns one non-blocking IPv4 socket and closes it on destruction. Move only.
// Every socket planck opens is non-blocking and close-on-exec. No call here
// ever sleeps, except tcp_connect() while it establishes the connection.
class Socket {
public:
    Socket() noexcept = default;
    ~Socket() noexcept { close(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(other.fd_), stream_(other.stream_) { other.fd_ = -1; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            stream_ = other.stream_;
            other.fd_ = -1;
        }
        return *this;
    }

    // Take ownership of a descriptor. stream says whether it is TCP, which
    // decides how a zero-byte read is reported.
    [[nodiscard]] static Socket adopt(int fd, bool stream) noexcept {
        Socket socket;
        socket.fd_ = fd;
        socket.stream_ = stream;
        return socket;
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int fd() const noexcept { return fd_; }
    void close() noexcept;

    // Read into buffer. On TCP, end of stream is reported as Closed.
    [[nodiscard]] IoResult receive(std::span<std::byte> buffer) noexcept;
    // Write from buffer. On TCP this may send only part of it.
    [[nodiscard]] IoResult send(std::span<const std::byte> buffer) noexcept;

    // UDP only: one datagram, with the sender's address.
    [[nodiscard]] IoResult receive_from(std::span<std::byte> buffer, Endpoint& from) noexcept;
    // UDP only: one datagram to an explicit destination.
    [[nodiscard]] IoResult send_to(std::span<const std::byte> buffer, Endpoint to) noexcept;

    // The address and port the socket is bound to. Useful after binding to
    // port 0, where the kernel picks the port.
    [[nodiscard]] core::Status local_endpoint(Endpoint& out) const noexcept;

private:
    int fd_ = -1;
    bool stream_ = false;
};

// ---- TCP ----

// Listen for connections. SO_REUSEADDR is set so a restart can rebind at once.
[[nodiscard]] core::Status tcp_listen(Endpoint local, Socket& out, int backlog = 128) noexcept;

// Take one pending connection. WouldBlock if none is waiting. The new socket
// has TCP_NODELAY set: planck sends small messages and never wants them held
// back to be merged.
[[nodiscard]] core::Status tcp_accept(const Socket& listener, Socket& out, Endpoint& peer) noexcept;

// Connect to a server. Blocks until the connection is made or refused, then
// returns a non-blocking socket with TCP_NODELAY. For clients and tools.
[[nodiscard]] core::Status tcp_connect(Endpoint remote, Socket& out) noexcept;

// ---- UDP ----

// A socket for sending to one fixed destination with send(). If destination
// is a multicast group, ttl limits how many routers the datagrams may cross
// (1 keeps them on the local network) and loopback controls whether
// receivers on this same host see them.
[[nodiscard]] core::Status udp_open_sender(Endpoint destination, Socket& out, std::uint8_t ttl = 1,
                                           bool loopback = true) noexcept;

// A socket for receiving on a port. If local.address is a multicast group,
// the socket joins it. Several receivers on one host may share the port.
[[nodiscard]] core::Status udp_open_receiver(Endpoint local, Socket& out) noexcept;

}  // namespace planck::net

#endif  // PLANCK_NET_SOCKET_HPP
