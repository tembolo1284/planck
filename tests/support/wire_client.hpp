#ifndef PLANCK_TESTS_SUPPORT_WIRE_CLIENT_HPP
#define PLANCK_TESTS_SUPPORT_WIRE_CLIENT_HPP

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <functional>
#include <span>
#include <utility>

#include <unistd.h>

#include "planck/net/socket.hpp"
#include "planck/wire/codec.hpp"

namespace planck::test {

// A minimal order entry client for tests: one TCP connection speaking the
// planck wire protocol.
//
// It never blocks. While it waits for a reply it keeps calling `pump`, which
// a single-threaded test uses to run the server (gateway.poll(), shard.poll())
// and a multi-threaded test leaves empty.
class WireClient {
public:
    explicit WireClient(std::function<void()> pump = [] {}) : pump_(std::move(pump)) {}

    void connect(net::Endpoint server) {
        ASSERT_EQ(net::tcp_connect(server, socket_), core::Status::Ok);
    }

    void close() { socket_.close(); }

    void send_bytes(std::span<const std::byte> bytes) {
        std::size_t done = 0;
        for (int attempt = 0; attempt < kMaxAttempts && done < bytes.size(); ++attempt) {
            const net::IoResult result = socket_.send(bytes.subspan(done));
            if (result.status == core::Status::Ok) {
                done += result.bytes;
            } else {
                ASSERT_EQ(result.status, core::Status::WouldBlock);
                wait();
            }
        }
        ASSERT_EQ(done, bytes.size()) << "could not send everything";
    }

    template <wire::Message T>
    void send(const T& message) {
        send_bytes({reinterpret_cast<const std::byte*>(&message), sizeof(T)});
    }

    // Log in and return the session id the gateway answered with.
    std::uint16_t login(std::uint32_t client_id, std::uint64_t token = 0) {
        wire::LoginRequest request{};
        request.client_id = client_id;
        request.token = token;
        send(request);
        return expect<wire::LoginAccepted>().session_id;
    }

    // The next message from the server, which must be a T.
    template <wire::Message T>
    T expect() {
        T message{};
        const wire::Frame frame = next();
        EXPECT_EQ(frame.status, core::Status::Ok) << "no reply while waiting for "
                                                  << wire::to_string(T::kType);
        if (frame.status == core::Status::Ok) {
            EXPECT_EQ(frame.type, T::kType) << "got " << wire::to_string(frame.type) << ", wanted "
                                            << wire::to_string(T::kType);
            if (frame.type == T::kType) {
                EXPECT_EQ(wire::decode(frame.bytes, message), core::Status::Ok);
            }
            consume(frame.bytes.size());
        }
        return message;
    }

    // True if the next message is a T. Gives one a short while to arrive.
    template <wire::Message T>
    bool pending() {
        for (int attempt = 0; attempt < kQuietAttempts; ++attempt) {
            wait();
            fill();
            const wire::Frame frame = wire::next_frame({buffer_.data(), length_});
            if (frame.status == core::Status::Ok) {
                return frame.type == T::kType;
            }
        }
        return false;
    }

    // True if no message arrives for a short while.
    bool quiet() {
        for (int attempt = 0; attempt < kQuietAttempts; ++attempt) {
            wait();
            fill();
            if (wire::next_frame({buffer_.data(), length_}).status != core::Status::WouldBlock) {
                return false;
            }
        }
        return true;
    }

    // True once the server has closed the connection.
    bool closed_by_server() {
        for (int attempt = 0; attempt < kMaxAttempts && !closed_; ++attempt) {
            wait();
            fill();
        }
        return closed_;
    }

private:
    static constexpr int kMaxAttempts = 20'000;
    static constexpr int kQuietAttempts = 200;

    void wait() {
        pump_();
        ::usleep(50);
    }

    // Pull whatever has arrived into the buffer.
    void fill() {
        if (closed_ || !socket_.valid() || length_ == buffer_.size()) {
            return;
        }
        const net::IoResult result =
            socket_.receive({buffer_.data() + length_, buffer_.size() - length_});
        if (result.status == core::Status::Ok) {
            length_ += result.bytes;
        } else if (result.status != core::Status::WouldBlock) {
            closed_ = true;
        }
    }

    wire::Frame next() {
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            const wire::Frame frame = wire::next_frame({buffer_.data(), length_});
            if (frame.status != core::Status::WouldBlock || closed_) {
                return frame;
            }
            wait();
            fill();
        }
        return wire::Frame{};
    }

    void consume(std::size_t bytes) {
        length_ -= bytes;
        std::memmove(buffer_.data(), buffer_.data() + bytes, length_);
    }

    std::function<void()> pump_;
    net::Socket socket_;
    std::array<std::byte, 16384> buffer_{};
    std::size_t length_ = 0;
    bool closed_ = false;
};

}  // namespace planck::test

#endif  // PLANCK_TESTS_SUPPORT_WIRE_CLIENT_HPP
