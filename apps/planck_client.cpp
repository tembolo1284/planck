// planck-client: an order entry client you can type at, or feed a script.
//
// It reads one command per line from standard input, sends it, and prints
// every message the venue sends back. It waits for the venue's answer to
// each request before reading the next line, so a script runs in order and
// "cancel last" always knows which order is meant.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include <poll.h>
#include <unistd.h>

#include "planck/core/types.hpp"
#include "planck/net/socket.hpp"
#include "planck/sys/clock.hpp"
#include "planck/wire/codec.hpp"
#include "support.hpp"

namespace {

using planck::core::ok;
using planck::core::Status;
namespace apps = planck::apps;
namespace core = planck::core;
namespace net = planck::net;
namespace sys = planck::sys;
namespace wire = planck::wire;

constexpr char kUsage[] =
    "planck-client: send orders to a planck venue\n"
    "\n"
    "  --connect ADDRESS:PORT  the venue's order entry port  (127.0.0.1:9000)\n"
    "  --client-id N           who to log in as, 1 to 65535  (1)\n"
    "  --token N               the secret the venue holds for that client id, if it\n"
    "                          was started with --clients    (0)\n"
    "  --linger MS             after the input ends, keep listening until the venue\n"
    "                          has been quiet this long      (200)\n"
    "\n"
    "Commands, one per line on standard input:\n";

constexpr char kCommands[] =
    "  buy  SYMBOL QTY PRICE [day|ioc|fok]   limit order. PRICE is in ticks.\n"
    "  sell SYMBOL QTY PRICE [day|ioc|fok]\n"
    "  buy  SYMBOL QTY market                market order\n"
    "  sell SYMBOL QTY market\n"
    "  cancel ORDER_ID                       the order_id from an 'accepted' line\n"
    "  cancel last                           the most recently accepted order\n"
    "  ping                                  heartbeat, echoed by the venue\n"
    "  wait MS                               pause, for scripts\n"
    "  help\n"
    "  quit\n";

constexpr std::uint64_t kNanosPerMilli = 1'000'000;
// How long the venue gets to answer a request before the client gives up.
constexpr std::uint64_t kAnswerTimeoutNs = 5'000 * kNanosPerMilli;

enum ExitCode : int { kOk = 0, kFailed = 1, kBadUsage = 2 };

// What the client is waiting for before it will read another command.
enum class Awaiting : std::uint8_t { Nothing, Login, OrderAnswer, CancelAnswer, Heartbeat };

std::string_view text_of(wire::LoginRejectReason reason) noexcept {
    switch (reason) {
        case wire::LoginRejectReason::BadCredentials:
            return "BadCredentials";
        case wire::LoginRejectReason::AlreadyLoggedIn:
            return "AlreadyLoggedIn";
        case wire::LoginRejectReason::UnsupportedVersion:
            return "UnsupportedVersion";
        case wire::LoginRejectReason::ServerFull:
            return "ServerFull";
    }
    return "Unknown";
}

std::string_view text_of(wire::CancelReason reason) noexcept {
    switch (reason) {
        case wire::CancelReason::UserRequested:
            return "requested";
        case wire::CancelReason::Unfilled:
            return "unfilled";
        case wire::CancelReason::Disconnected:
            return "disconnected";
    }
    return "unknown";
}

class Client {
public:
    Client(std::uint32_t client_id, std::uint64_t token, std::uint64_t linger_ms) noexcept
        : client_id_(client_id), token_(token), linger_ns_(linger_ms * kNanosPerMilli) {}

    [[nodiscard]] int run(net::Endpoint server, const std::atomic<bool>& stop) noexcept;

private:
    static constexpr std::size_t kRxBytes = 16384;
    static constexpr std::size_t kLineBytes = 1024;

    [[nodiscard]] static std::uint64_t now() noexcept { return sys::monotonic_now().raw(); }

    template <wire::Message T>
    [[nodiscard]] bool send(const T& message, Awaiting answer) noexcept;
    // Read from the socket and print every complete message. False if the
    // connection is gone or the venue sent something that is not the protocol.
    [[nodiscard]] bool receive() noexcept;
    void print(const wire::Frame& frame) noexcept;
    void answered(Awaiting what) noexcept;

    // Read from standard input into the line buffer.
    void read_input() noexcept;
    // Run buffered commands until one has to wait for an answer.
    void run_commands() noexcept;
    void run_command(std::string_view line) noexcept;
    void new_order(core::Side side, std::span<const std::string_view> words) noexcept;
    void cancel(std::span<const std::string_view> words) noexcept;

    [[nodiscard]] int poll_timeout_ms(std::uint64_t at) const noexcept;

    net::Socket socket_{};
    std::uint32_t client_id_;
    std::uint64_t token_;
    std::uint64_t linger_ns_;
    Awaiting awaiting_ = Awaiting::Nothing;
    std::uint64_t sent_at_ = 0;        // when the request being awaited went out
    std::uint64_t last_activity_ = 0;  // last send or receive
    std::uint64_t paused_until_ = 0;   // set by "wait"
    std::uint64_t next_client_order_id_ = 1;
    std::uint64_t last_order_id_ = 0;
    bool input_open_ = true;
    bool quit_ = false;
    bool failed_ = false;
    std::size_t rx_length_ = 0;
    std::size_t line_length_ = 0;
    std::array<std::byte, kRxBytes> rx_{};
    std::array<char, kLineBytes> line_{};
};

template <wire::Message T>
bool Client::send(const T& message, Awaiting answer) noexcept {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(&message), sizeof(T)};
    std::size_t done = 0;
    // A message is at most 48 bytes, so this almost always takes one pass.
    for (int attempt = 0; attempt < 1000 && done < bytes.size(); ++attempt) {
        const net::IoResult result = socket_.send(bytes.subspan(done));
        if (result.status == Status::Ok) {
            done += result.bytes;
        } else if (result.status == Status::WouldBlock) {
            pollfd waiting{.fd = socket_.fd(), .events = POLLOUT, .revents = 0};
            static_cast<void>(::poll(&waiting, 1, 10));
        } else {
            break;
        }
    }
    if (done != bytes.size()) {
        std::fprintf(stderr, "planck-client: could not send, the connection is gone\n");
        failed_ = true;
        return false;
    }
    sent_at_ = now();
    last_activity_ = sent_at_;
    awaiting_ = answer;
    return true;
}

void Client::answered(Awaiting what) noexcept {
    if (awaiting_ == what) {
        const double micros = static_cast<double>(now() - sent_at_) / 1000.0;
        std::printf("  round trip %.1f us\n", micros);
        awaiting_ = Awaiting::Nothing;
    }
}

void Client::print(const wire::Frame& frame) noexcept {
    const auto u64 = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    switch (frame.type) {
        case wire::MsgType::LoginAccepted: {
            wire::LoginAccepted m;
            if (ok(wire::decode(frame.bytes, m))) {
                std::printf("logged in client_id=%u session=%u\n", client_id_, m.session_id);
                awaiting_ = (awaiting_ == Awaiting::Login) ? Awaiting::Nothing : awaiting_;
            }
            break;
        }
        case wire::MsgType::LoginRejected: {
            wire::LoginRejected m;
            if (ok(wire::decode(frame.bytes, m))) {
                const std::string_view reason = text_of(m.reason);
                std::printf("login rejected reason=%.*s\n", static_cast<int>(reason.size()),
                            reason.data());
                failed_ = true;
            }
            break;
        }
        case wire::MsgType::OrderAccepted: {
            wire::OrderAccepted m;
            if (ok(wire::decode(frame.bytes, m))) {
                std::printf("accepted client_order_id=%llu order_id=%llu time=%s\n",
                            u64(m.client_order_id), u64(m.order_id),
                            apps::format_time(m.timestamp).c_str());
                last_order_id_ = m.order_id;
                answered(Awaiting::OrderAnswer);
            }
            break;
        }
        case wire::MsgType::OrderRejected: {
            wire::OrderRejected m;
            if (ok(wire::decode(frame.bytes, m))) {
                const std::string_view reason = wire::to_string(m.reason);
                std::printf("rejected client_order_id=%llu reason=%.*s\n", u64(m.client_order_id),
                            static_cast<int>(reason.size()), reason.data());
                answered(Awaiting::OrderAnswer);
            }
            break;
        }
        case wire::MsgType::Executed: {
            wire::Executed m;
            if (ok(wire::decode(frame.bytes, m))) {
                std::printf("executed order_id=%llu qty=%u price=%d leaves=%u %s match_id=%llu "
                            "time=%s\n",
                            u64(m.order_id), m.qty, m.price, m.leaves,
                            m.liquidity == wire::Liquidity::Maker ? "maker" : "taker",
                            u64(m.match_id), apps::format_time(m.timestamp).c_str());
            }
            break;
        }
        case wire::MsgType::Canceled: {
            wire::Canceled m;
            if (ok(wire::decode(frame.bytes, m))) {
                const std::string_view reason = text_of(m.reason);
                std::printf("canceled order_id=%llu qty=%u reason=%.*s time=%s\n", u64(m.order_id),
                            m.qty, static_cast<int>(reason.size()), reason.data(),
                            apps::format_time(m.timestamp).c_str());
                // The unfilled rest of an IOC order also arrives as a
                // Canceled. Only one that was asked for answers a cancel.
                if (m.reason == wire::CancelReason::UserRequested) {
                    answered(Awaiting::CancelAnswer);
                }
            }
            break;
        }
        case wire::MsgType::CancelRejected: {
            wire::CancelRejected m;
            if (ok(wire::decode(frame.bytes, m))) {
                const std::string_view reason = wire::to_string(m.reason);
                std::printf("cancel rejected order_id=%llu reason=%.*s\n", u64(m.order_id),
                            static_cast<int>(reason.size()), reason.data());
                answered(Awaiting::CancelAnswer);
            }
            break;
        }
        case wire::MsgType::Heartbeat:
            std::printf("heartbeat\n");
            answered(Awaiting::Heartbeat);
            break;
        default: {
            const std::string_view name = wire::to_string(frame.type);
            std::printf("unexpected message %.*s\n", static_cast<int>(name.size()), name.data());
            break;
        }
    }
}

bool Client::receive() noexcept {
    const net::IoResult result =
        socket_.receive({rx_.data() + rx_length_, rx_.size() - rx_length_});
    if (result.status == Status::WouldBlock) {
        return true;
    }
    if (!ok(result.status)) {
        std::fprintf(stderr, "planck-client: the venue closed the connection\n");
        return false;
    }
    rx_length_ += result.bytes;
    last_activity_ = now();

    std::size_t offset = 0;
    // Each pass consumes at least a header, so the buffer size bounds this.
    while (offset < rx_length_) {
        const wire::Frame frame = wire::next_frame({rx_.data() + offset, rx_length_ - offset});
        if (frame.status == Status::WouldBlock) {
            break;
        }
        if (!ok(frame.status)) {
            std::fprintf(stderr, "planck-client: the venue sent bytes that are not the protocol\n");
            return false;
        }
        print(frame);
        offset += frame.bytes.size();
    }
    rx_length_ -= offset;
    if (rx_length_ != 0 && offset != 0) {
        std::memmove(rx_.data(), rx_.data() + offset, rx_length_);
    }
    return true;
}

void Client::read_input() noexcept {
    std::array<char, 256> chunk{};
    const ssize_t got = ::read(STDIN_FILENO, chunk.data(), chunk.size());
    if (got > 0) {
        const auto bytes = static_cast<std::size_t>(got);
        if (bytes > (line_.size() - line_length_)) {
            std::fprintf(stderr, "planck-client: line too long, ignoring it\n");
            line_length_ = 0;
            return;
        }
        std::memcpy(line_.data() + line_length_, chunk.data(), bytes);
        line_length_ += bytes;
    } else if (got == 0) {
        input_open_ = false;
        // A last line with no newline still counts.
        if (line_length_ != 0 && line_length_ < line_.size()) {
            line_[line_length_] = '\n';
            ++line_length_;
        }
    }
}

void Client::run_commands() noexcept {
    // Each pass removes one line from the buffer, so its size bounds this.
    while (awaiting_ == Awaiting::Nothing && paused_until_ == 0 && !quit_ && !failed_) {
        const std::string_view buffered{line_.data(), line_length_};
        const std::size_t end = buffered.find('\n');
        if (end == std::string_view::npos) {
            break;
        }
        // Copy the line out before shifting the buffer under it.
        std::array<char, kLineBytes> line{};
        std::memcpy(line.data(), line_.data(), end);
        line_length_ -= end + 1;
        std::memmove(line_.data(), line_.data() + end + 1, line_length_);
        run_command({line.data(), end});
    }
}

void Client::new_order(core::Side side, std::span<const std::string_view> words) noexcept {
    wire::NewOrder order{};
    order.side = std::to_underlying(side);
    const bool sized = (words.size() == 4 || words.size() == 5) &&
                       apps::parse_number<std::uint16_t>(words[1], order.symbol_id, 0, 65535) &&
                       apps::parse_number<std::uint32_t>(words[2], order.qty, 0, 0xFFFFFFFFU);
    bool priced = false;
    if (sized && words[3] == "market") {
        order.order_type = std::to_underlying(core::OrderType::Market);
        order.tif = std::to_underlying(core::TimeInForce::ImmediateOrCancel);
        priced = (words.size() == 4);
    } else if (sized) {
        priced = apps::parse_number<std::int32_t>(words[3], order.price, INT32_MIN, INT32_MAX);
        const std::string_view tif = (words.size() == 5) ? words[4] : "day";
        if (tif == "ioc") {
            order.tif = std::to_underlying(core::TimeInForce::ImmediateOrCancel);
        } else if (tif == "fok") {
            order.tif = std::to_underlying(core::TimeInForce::FillOrKill);
        } else if (tif != "day") {
            priced = false;
        }
    }
    if (!priced) {
        std::printf("usage: buy|sell SYMBOL QTY PRICE [day|ioc|fok]   or   buy|sell SYMBOL QTY "
                    "market\n");
        return;
    }
    order.client_order_id = next_client_order_id_;
    ++next_client_order_id_;
    static_cast<void>(send(order, Awaiting::OrderAnswer));
}

void Client::cancel(std::span<const std::string_view> words) noexcept {
    wire::CancelOrder message{};
    bool usable = (words.size() == 2);
    if (usable && words[1] == "last") {
        if (last_order_id_ == 0) {
            std::printf("no order has been accepted yet\n");
            return;
        }
        message.order_id = last_order_id_;
    } else if (usable) {
        usable = apps::parse_number<std::uint64_t>(words[1], message.order_id, 0, UINT64_MAX);
    }
    if (!usable) {
        std::printf("usage: cancel ORDER_ID   or   cancel last\n");
        return;
    }
    static_cast<void>(send(message, Awaiting::CancelAnswer));
}

void Client::run_command(std::string_view line) noexcept {
    std::array<std::string_view, 8> storage{};
    const std::size_t count = apps::split_words(line, storage);
    if (count == 0 || storage[0].starts_with('#')) {
        return;  // blank line or comment
    }
    if (count > storage.size()) {
        std::printf("too many words (try help)\n");
        return;
    }
    const std::span<const std::string_view> words{storage.data(), count};
    const std::string_view verb = words[0];

    if (verb == "buy") {
        new_order(core::Side::Buy, words);
    } else if (verb == "sell") {
        new_order(core::Side::Sell, words);
    } else if (verb == "cancel") {
        cancel(words);
    } else if (verb == "ping") {
        static_cast<void>(send(wire::Heartbeat{}, Awaiting::Heartbeat));
    } else if (verb == "wait") {
        std::uint64_t ms = 0;
        if (count == 2 && apps::parse_number<std::uint64_t>(words[1], ms, 1, 600'000)) {
            paused_until_ = now() + (ms * kNanosPerMilli);
        } else {
            std::printf("usage: wait MS (1 to 600000)\n");
        }
    } else if (verb == "help") {
        std::fputs(kCommands, stdout);
    } else if (verb == "quit" || verb == "exit") {
        quit_ = true;
    } else {
        std::printf("unknown command '%.*s' (try help)\n", static_cast<int>(verb.size()),
                    verb.data());
    }
}

int Client::poll_timeout_ms(std::uint64_t at) const noexcept {
    // Wake for whichever comes first: the end of a pause, the answer
    // deadline, or the end of the linger. Never sleep longer than 100 ms.
    std::uint64_t wait_ns = 100 * kNanosPerMilli;
    const auto sooner = [&wait_ns, at](std::uint64_t deadline) {
        const std::uint64_t left = (deadline > at) ? (deadline - at) : 0;
        wait_ns = (left < wait_ns) ? left : wait_ns;
    };
    if (paused_until_ != 0) {
        sooner(paused_until_);
    }
    if (awaiting_ != Awaiting::Nothing) {
        sooner(sent_at_ + kAnswerTimeoutNs);
    }
    if (!input_open_) {
        sooner(last_activity_ + linger_ns_);
    }
    // Round up, so a deadline 0.2 ms away does not turn into a busy loop.
    return static_cast<int>((wait_ns + kNanosPerMilli - 1) / kNanosPerMilli);
}

int Client::run(net::Endpoint server, const std::atomic<bool>& stop) noexcept {
    if (const Status status = net::tcp_connect(server, socket_); !ok(status)) {
        const std::string_view reason = core::to_string(status);
        std::fprintf(stderr, "planck-client: cannot connect: %.*s\n",
                     static_cast<int>(reason.size()), reason.data());
        return kFailed;
    }
    wire::LoginRequest login{};
    login.client_id = client_id_;
    login.token = token_;
    if (!send(login, Awaiting::Login)) {
        return kFailed;
    }

    while (!stop.load(std::memory_order_relaxed) && !quit_ && !failed_) {
        const std::uint64_t at = now();
        if (paused_until_ != 0 && at >= paused_until_) {
            paused_until_ = 0;
        }
        if (awaiting_ != Awaiting::Nothing && (at - sent_at_) > kAnswerTimeoutNs) {
            std::fprintf(stderr, "planck-client: no answer from the venue after 5 seconds\n");
            return kFailed;
        }
        run_commands();
        const bool idle = (awaiting_ == Awaiting::Nothing) && (paused_until_ == 0);
        const bool input_done = !input_open_ && (line_length_ == 0);
        if (idle && input_done && (at - last_activity_) >= linger_ns_) {
            break;  // nothing left to send and the venue has gone quiet
        }

        // Only listen to the keyboard when ready for another command.
        const bool want_input = input_open_ && idle;
        std::array<pollfd, 2> fds{{{.fd = socket_.fd(), .events = POLLIN, .revents = 0},
                                   {.fd = want_input ? STDIN_FILENO : -1,
                                    .events = POLLIN,
                                    .revents = 0}}};
        static_cast<void>(::poll(fds.data(), fds.size(), poll_timeout_ms(at)));
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 && !receive()) {
            return kFailed;
        }
        if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
            read_input();
        }
    }
    if (failed_) {
        return kFailed;
    }
    // Leave politely. If this fails the venue notices the close anyway.
    static_cast<void>(send(wire::Logout{}, Awaiting::Nothing));
    return kOk;
}

}  // namespace

int main(int argc, char** argv) {
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    apps::Args args(argc, argv);
    if (args.flag("--help")) {
        std::fputs(kUsage, stdout);
        std::fputs(kCommands, stdout);
        return kOk;
    }
    net::Endpoint server{net::kLoopback, apps::kDefaultOrderPort};
    std::uint32_t client_id = 1;
    std::uint64_t token = 0;
    std::uint64_t linger_ms = 200;
    if (!args.endpoint("--connect", server) ||
        !args.number<std::uint32_t>("--client-id", client_id, 1, 0xFFFFU) ||
        !args.number<std::uint64_t>("--token", token, 0, ~std::uint64_t{0}) ||
        !args.number<std::uint64_t>("--linger", linger_ms, 0, 600'000) || !args.finish()) {
        return kBadUsage;
    }

    // Too big for comfort on the stack, and there is only ever one.
    static Client client(client_id, token, linger_ms);
    return client.run(server, apps::stop_on_signal());
}
