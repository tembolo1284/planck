// planck-loadgen: drive a running planckd over TCP and measure how long it
// takes to answer, as a client sees it.
//
// It logs in a number of sessions and sends new orders through them. Most
// rest on the book. Some cross and trade. Each session cancels its oldest
// resting order whenever it holds more than --depth, so the book stays the
// same size however long the run. The time from sending each new order to
// reading its OrderAccepted or OrderRejected goes into a histogram.
//
// Two ways to pace the load:
//
//   closed loop (the default)  each session keeps --window orders
//       unanswered and sends the next as soon as one is answered. Measures
//       the round trip with nothing queued. Says nothing about overload.
//
//   fixed rate (--rate N)      N orders a second on a schedule fixed in
//       advance. Each answer is also measured against the moment its order
//       was due, so time spent waiting behind a slow venue counts.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include <unistd.h>

#include "flow.hpp"
#include "harness.hpp"
#include "planck/core/config.hpp"
#include "planck/core/hint.hpp"
#include "planck/net/socket.hpp"
#include "planck/perf/histogram.hpp"
#include "planck/sys/cpu.hpp"
#include "planck/wire/codec.hpp"
#include "support.hpp"

namespace {

using planck::bench::Fifo;
using planck::bench::Pacer;
using planck::bench::Rng;
using planck::bench::SendLog;
using planck::bench::SendTimes;
using planck::bench::Stopwatch;
using planck::bench::Table;
using planck::core::ok;
using planck::core::Status;
using planck::perf::Histogram;
namespace apps = planck::apps;
namespace core = planck::core;
namespace net = planck::net;
namespace sys = planck::sys;
namespace wire = planck::wire;

constexpr char kUsage[] =
    "planck-loadgen: send orders to a running planckd and time the answers\n"
    "\n"
    "  --server ADDRESS:PORT   where planckd listens            (127.0.0.1:9000)\n"
    "  --sessions N            connections to open, 1 to 64     (1)\n"
    "  --orders N              new orders to send and time      (100000)\n"
    "  --warmup N              orders sent first and not timed  (1000)\n"
    "  --rate N                orders a second on a fixed schedule. Without it,\n"
    "                          each session sends as fast as it is answered.\n"
    "  --window N              closed loop: unanswered orders per session (1)\n"
    "  --cross PERCENT         share of orders that cross and trade (10)\n"
    "  --depth N               resting orders each session keeps (1000)\n"
    "  --symbols N             trade symbols 1 to N             (1)\n"
    "  --band-low TICKS        as given to planckd              (0)\n"
    "  --band-ticks N          as given to planckd              (65536)\n"
    "  --client-id N           first client id to log in as     (1000)\n"
    "  --cpu N                 pin to this CPU\n"
    "  --csv                   comma separated output\n";

constexpr std::size_t kMaxSessions = 64;
constexpr std::size_t kMaxWindow = 1U << 12;
constexpr std::size_t kMaxDepth = 1U << 16;
// Orders rest this many ticks either side of the middle of the band.
constexpr std::uint32_t kSpread = 16;
constexpr std::uint64_t kPatienceNanos = 10'000'000'000ULL;
// Remembered with each order: will it rest if accepted?
constexpr std::uint64_t kResting = 1;
constexpr std::uint64_t kCrossing = 2;

struct Options {
    net::Endpoint server{net::kLoopback, apps::kDefaultOrderPort};
    std::uint32_t sessions = 1;
    std::uint64_t orders = 100'000;
    std::uint64_t warmup = 1'000;
    std::uint64_t rate = 0;
    std::uint32_t window = 1;
    std::uint32_t cross_percent = 10;
    std::uint32_t depth = 1'000;
    std::uint32_t symbols = 1;
    std::int32_t band_low = 0;
    std::uint32_t band_ticks = 1U << 16;
    std::uint32_t client_id = 1'000;
    std::size_t cpu = sys::kMaxCpus;
    bool csv = false;
};

struct Totals {
    std::uint64_t sent = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t busy = 0;
    std::uint64_t fills = 0;
    std::uint64_t cancels_sent = 0;
    std::uint64_t cancels_done = 0;
    // The order had already traded away. Expected now and then.
    std::uint64_t cancels_too_late = 0;
    std::uint64_t unexpected = 0;
    // The most new orders any one session had unanswered at once.
    std::uint32_t most_unanswered = 0;
};

struct Session {
    net::Socket socket;
    std::uint64_t next_client_order_id = 1;
    std::uint32_t unanswered = 0;
    std::uint32_t cancels_pending = 0;
    bool alive = false;
    std::size_t rx_length = 0;
    std::size_t tx_length = 0;
    std::array<std::byte, 1U << 16> rx{};
    std::array<std::byte, 1U << 16> tx{};
    SendLog<kMaxWindow> log;
    Fifo<kMaxDepth> resting;
};

class LoadGenerator {
public:
    LoadGenerator(const Options& options, const Stopwatch& watch) noexcept
        : options_(options), watch_(watch), rng_(options.client_id) {}

    [[nodiscard]] bool connect() noexcept;
    [[nodiscard]] bool run() noexcept;
    void print() const noexcept;

private:
    [[nodiscard]] bool room_to_send(const Session& session) const noexcept {
        return session.alive && session.unanswered < kMaxWindow &&
               (session.tx.size() - session.tx_length) >= (2 * wire::kMaxMessageSize);
    }

    template <wire::Message T>
    void queue(Session& session, const T& message) noexcept {
        std::memcpy(session.tx.data() + session.tx_length, &message, sizeof(T));
        session.tx_length += sizeof(T);
    }

    void send_order(Session& session, std::uint64_t intended) noexcept;
    void send_cancel(Session& session, std::uint64_t order_id) noexcept;
    void flush(Session& session) noexcept;
    // Read and handle whatever has arrived. Returns how many bytes that was.
    std::size_t receive(Session& session) noexcept;
    void handle(Session& session, const wire::Frame& frame, std::uint64_t now) noexcept;
    void drop(Session& session, const char* why) noexcept;

    [[nodiscard]] std::uint64_t unanswered() const noexcept;
    [[nodiscard]] std::uint64_t cancels_pending() const noexcept;
    // Spin, reading every session, until `done` says so or patience runs out.
    template <typename Done>
    [[nodiscard]] bool settle(Done done) noexcept;

    const Options& options_;
    const Stopwatch& watch_;
    Rng rng_;
    Totals totals_{};
    std::uint64_t started_tsc_ = 0;
    std::uint64_t finished_tsc_ = 0;
    std::uint32_t alive_ = 0;
    std::uint32_t logins_ = 0;
    // New orders answered so far, warm-up included.
    std::uint64_t answered_ = 0;
    // Send to answer.
    Histogram service_{};
    // Due time to answer. Fixed rate only.
    Histogram response_{};
    std::array<Session, kMaxSessions> sessions_{};
};

bool LoadGenerator::connect() noexcept {
    for (std::uint32_t i = 0; i < options_.sessions; ++i) {
        Session& session = sessions_[i];
        if (const Status status = net::tcp_connect(options_.server, session.socket); !ok(status)) {
            const std::string_view why = core::to_string(status);
            std::fprintf(stderr, "planck-loadgen: cannot connect to %s: %.*s\n",
                         apps::format_endpoint(options_.server).c_str(),
                         static_cast<int>(why.size()), why.data());
            return false;
        }
        session.alive = true;
        ++alive_;
        wire::LoginRequest login{};
        login.client_id = options_.client_id + i;
        queue(session, login);
        flush(session);
    }

    // Wait for every LoginAccepted. handle() counts them as they come.
    const bool good = settle([this] { return logins_ == options_.sessions; });
    if (!good || alive_ != options_.sessions) {
        std::fprintf(stderr, "planck-loadgen: %u of %u sessions logged in\n", logins_,
                     options_.sessions);
        return false;
    }
    return true;
}

void LoadGenerator::send_order(Session& session, std::uint64_t intended) noexcept {
    const std::int32_t mid =
        options_.band_low + static_cast<std::int32_t>(options_.band_ticks / 2);
    const bool buy = (rng_.next() & 1U) != 0;
    const bool cross = rng_.below(100) < options_.cross_percent;
    const auto away = static_cast<std::int32_t>(1 + rng_.below(kSpread));

    wire::NewOrder order{};
    order.symbol_id = static_cast<std::uint16_t>(1 + rng_.below(options_.symbols));
    order.side = std::to_underlying(buy ? core::Side::Buy : core::Side::Sell);
    order.order_type = std::to_underlying(core::OrderType::Limit);
    order.qty = 1 + rng_.below(10);
    order.client_order_id = session.next_client_order_id++;
    if (cross) {
        // Priced through the far side of the resting orders, and gone at
        // once if nothing is there, so it never rests.
        order.tif = std::to_underlying(core::TimeInForce::ImmediateOrCancel);
        order.price = buy ? (mid + static_cast<std::int32_t>(kSpread)) : (mid - static_cast<std::int32_t>(kSpread));
    } else {
        order.tif = std::to_underlying(core::TimeInForce::Day);
        order.price = buy ? (mid - away) : (mid + away);
    }

    queue(session, order);
    const std::uint64_t now = sys::tsc_now();
    const SendTimes times{.sent = now,
                          .intended = (intended != 0) ? intended : now,
                          .tag = cross ? kCrossing : kResting};
    if (!session.log.put(order.client_order_id, times)) {
        drop(session, "too many orders unanswered");
        return;
    }
    ++session.unanswered;
    totals_.most_unanswered = std::max(totals_.most_unanswered, session.unanswered);
    ++totals_.sent;
    flush(session);
}

void LoadGenerator::send_cancel(Session& session, std::uint64_t order_id) noexcept {
    wire::CancelOrder cancel{};
    cancel.order_id = order_id;
    queue(session, cancel);
    ++session.cancels_pending;
    ++totals_.cancels_sent;
}

void LoadGenerator::flush(Session& session) noexcept {
    if (!session.alive || session.tx_length == 0) {
        return;
    }
    const net::IoResult result = session.socket.send({session.tx.data(), session.tx_length});
    if (ok(result.status)) {
        session.tx_length -= result.bytes;
        if (session.tx_length != 0) {
            std::memmove(session.tx.data(), session.tx.data() + result.bytes, session.tx_length);
        }
    } else if (result.status != Status::WouldBlock) {
        drop(session, "the venue closed the connection");
    }
}

std::size_t LoadGenerator::receive(Session& session) noexcept {
    if (!session.alive) {
        return 0;
    }
    const net::IoResult result = session.socket.receive(
        {session.rx.data() + session.rx_length, session.rx.size() - session.rx_length});
    if (result.status == Status::WouldBlock) {
        return 0;
    }
    if (!ok(result.status)) {
        drop(session, "the venue closed the connection");
        return 0;
    }
    // One reading for everything that arrived together.
    const std::uint64_t now = sys::tsc_now();
    session.rx_length += result.bytes;

    std::size_t offset = 0;
    while (session.alive && offset < session.rx_length) {
        const wire::Frame frame =
            wire::next_frame({session.rx.data() + offset, session.rx_length - offset});
        if (frame.status == Status::WouldBlock) {
            break;
        }
        if (!ok(frame.status)) {
            drop(session, "the venue sent bytes that are not the protocol");
            return result.bytes;
        }
        handle(session, frame, now);
        offset += frame.bytes.size();
    }
    session.rx_length -= offset;
    if (session.rx_length != 0 && offset != 0) {
        std::memmove(session.rx.data(), session.rx.data() + offset, session.rx_length);
    }
    // Cancels queued while handling go out with whatever else is waiting.
    flush(session);
    return result.bytes;
}

void LoadGenerator::handle(Session& session, const wire::Frame& frame, std::uint64_t now) noexcept {
    // Returns the tag the order was sent with, or zero if it was not ours.
    const auto answered = [&](std::uint64_t client_order_id) -> std::uint64_t {
        SendTimes times;
        if (!session.log.take(client_order_id, times)) {
            ++totals_.unexpected;
            return 0;
        }
        --session.unanswered;
        if (answered_ >= options_.warmup) {
            service_.record(watch_.adjust(now - times.sent));
            response_.record(watch_.adjust(now - times.intended));
        }
        ++answered_;
        return times.tag;
    };

    switch (frame.type) {
        case wire::MsgType::LoginAccepted: {
            // Carry on above the ids an earlier run used as this client, or
            // the venue would take every order for a repeat and ignore it.
            wire::LoginAccepted accepted;
            if (ok(wire::decode(frame.bytes, accepted))) {
                session.next_client_order_id = accepted.last_client_order_id + 1;
            }
            ++logins_;
            break;
        }
        case wire::MsgType::OrderAccepted: {
            wire::OrderAccepted accepted;
            static_cast<void>(wire::decode(frame.bytes, accepted));
            const std::uint64_t tag = answered(accepted.client_order_id);
            ++totals_.accepted;
            if (tag == kResting && !session.resting.push(accepted.order_id)) {
                ++totals_.unexpected;
            }
            while (session.resting.size() > options_.depth) {
                std::uint64_t oldest = 0;
                static_cast<void>(session.resting.pop(oldest));
                send_cancel(session, oldest);
            }
            break;
        }
        case wire::MsgType::OrderRejected: {
            wire::OrderRejected rejected;
            static_cast<void>(wire::decode(frame.bytes, rejected));
            static_cast<void>(answered(rejected.client_order_id));
            ++totals_.rejected;
            totals_.busy += (rejected.reason == wire::RejectCode::Busy) ? 1U : 0U;
            break;
        }
        case wire::MsgType::Executed:
            ++totals_.fills;
            break;
        case wire::MsgType::Canceled: {
            wire::Canceled canceled;
            static_cast<void>(wire::decode(frame.bytes, canceled));
            if (canceled.reason == wire::CancelReason::UserRequested) {
                --session.cancels_pending;
                ++totals_.cancels_done;
            }
            break;
        }
        case wire::MsgType::CancelRejected:
            --session.cancels_pending;
            ++totals_.cancels_too_late;
            break;
        case wire::MsgType::Heartbeat:
            break;
        default:
            ++totals_.unexpected;
            break;
    }
}

void LoadGenerator::drop(Session& session, const char* why) noexcept {
    if (!session.alive) {
        return;
    }
    std::fprintf(stderr, "planck-loadgen: session lost: %s\n", why);
    session.socket.close();
    session.alive = false;
    --alive_;
}

std::uint64_t LoadGenerator::unanswered() const noexcept {
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < options_.sessions; ++i) {
        total += sessions_[i].unanswered;
    }
    return total;
}

std::uint64_t LoadGenerator::cancels_pending() const noexcept {
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < options_.sessions; ++i) {
        total += sessions_[i].cancels_pending + sessions_[i].tx_length;
    }
    return total;
}

template <typename Done>
bool LoadGenerator::settle(Done done) noexcept {
    const std::uint64_t patience = watch_.from_nanos(kPatienceNanos);
    const std::uint64_t began = sys::tsc_now();
    while (!done()) {
        if (alive_ != options_.sessions || (sys::tsc_now() - began) > patience) {
            return false;
        }
        std::size_t bytes = 0;
        for (std::uint32_t i = 0; i < options_.sessions; ++i) {
            flush(sessions_[i]);
            bytes += receive(sessions_[i]);
        }
        if (bytes == 0) {
            core::cpu_relax();
        }
    }
    return true;
}

bool LoadGenerator::run() noexcept {
    const std::uint64_t total = options_.warmup + options_.orders;
    const std::uint64_t patience = watch_.from_nanos(kPatienceNanos);
    const bool fixed_rate = options_.rate != 0;
    const std::uint64_t interval = fixed_rate ? (watch_.ticks_per_second() / options_.rate) : 0;

    started_tsc_ = sys::tsc_now();
    Pacer pacer(started_tsc_, interval);
    std::uint64_t last_progress = started_tsc_;
    std::uint64_t timed_from = 0;
    std::uint32_t turn = 0;

    while (totals_.sent < total) {
        if (alive_ != options_.sessions) {
            return false;
        }
        const std::uint64_t before = totals_.sent + answered_;
        for (std::uint32_t i = 0; i < options_.sessions; ++i) {
            static_cast<void>(receive(sessions_[i]));
        }

        if (fixed_rate) {
            // Everything that has fallen due goes out now, round robin. If
            // no session has room, the order stays due and its wait counts.
            std::uint64_t intended = 0;
            Session& session = sessions_[turn % options_.sessions];
            if (totals_.sent < total && room_to_send(session) && pacer.due(sys::tsc_now(), intended)) {
                send_order(session, intended);
                ++turn;
            }
        } else {
            for (std::uint32_t i = 0; i < options_.sessions && totals_.sent < total; ++i) {
                Session& session = sessions_[i];
                if (session.unanswered < options_.window && room_to_send(session)) {
                    send_order(session, 0);
                }
            }
        }

        if (timed_from == 0 && answered_ >= options_.warmup) {
            timed_from = sys::tsc_now();
        }
        const std::uint64_t now = sys::tsc_now();
        if (totals_.sent + answered_ != before) {
            last_progress = now;
        } else if ((now - last_progress) > patience) {
            std::fprintf(stderr, "planck-loadgen: the venue stopped answering\n");
            return false;
        }
    }

    if (!settle([this] { return unanswered() == 0; })) {
        std::fprintf(stderr, "planck-loadgen: %llu orders were never answered\n",
                     static_cast<unsigned long long>(unanswered()));
        return false;
    }
    finished_tsc_ = sys::tsc_now();
    started_tsc_ = (timed_from != 0) ? timed_from : started_tsc_;

    // Leave the book as it was found, so that one run does not change what
    // the next one measures.
    for (std::uint32_t i = 0; i < options_.sessions; ++i) {
        Session& session = sessions_[i];
        std::uint64_t id = 0;
        while (session.resting.pop(id)) {
            while (!room_to_send(session)) {
                if (!settle([&] { return room_to_send(session); })) {
                    return false;
                }
            }
            send_cancel(session, id);
            flush(session);
        }
    }
    if (!settle([this] { return cancels_pending() == 0; })) {
        std::fprintf(stderr, "planck-loadgen: not every cancel was answered\n");
        return false;
    }
    return totals_.unexpected == 0;
}

void LoadGenerator::print() const noexcept {
    const double seconds = watch_.to_nanos(finished_tsc_ - started_tsc_) / 1e9;
    const std::uint64_t timed = service_.count();
    std::fprintf(stderr,
                 "# %u sessions to %s, %s, %llu orders timed in %.3f s (%.0f a second)\n",
                 options_.sessions, apps::format_endpoint(options_.server).c_str(),
                 (options_.rate != 0) ? "fixed rate" : "closed loop",
                 static_cast<unsigned long long>(timed), seconds,
                 (seconds > 0.0) ? (static_cast<double>(timed) / seconds) : 0.0);
    std::fprintf(stderr,
                 "# sent %llu, accepted %llu, rejected %llu (busy %llu), fills %llu, "
                 "cancels %llu (too late %llu), most unanswered %u, unexpected %llu\n",
                 static_cast<unsigned long long>(totals_.sent),
                 static_cast<unsigned long long>(totals_.accepted),
                 static_cast<unsigned long long>(totals_.rejected),
                 static_cast<unsigned long long>(totals_.busy),
                 static_cast<unsigned long long>(totals_.fills),
                 static_cast<unsigned long long>(totals_.cancels_done),
                 static_cast<unsigned long long>(totals_.cancels_too_late),
                 totals_.most_unanswered,
                 static_cast<unsigned long long>(totals_.unexpected));

    const Table table(watch_, options_.csv);
    table.header();
    table.row("order.sent_to_answer", service_);
    if (options_.rate != 0) {
        table.row("order.due_to_answer", response_);
    }
}

bool parse(int argc, const char* const* argv, Options& options) {
    apps::Args args(argc, argv);
    options.csv = args.flag("--csv");
    const bool good =
        args.endpoint("--server", options.server) &&
        args.number<std::uint32_t>("--sessions", options.sessions, 1, kMaxSessions) &&
        args.number<std::uint64_t>("--orders", options.orders, 1, 1'000'000'000'000ULL) &&
        args.number<std::uint64_t>("--warmup", options.warmup, 0, 1'000'000'000ULL) &&
        args.number<std::uint64_t>("--rate", options.rate, 1, 10'000'000) &&
        args.number<std::uint32_t>("--window", options.window, 1, kMaxWindow) &&
        args.number<std::uint32_t>("--cross", options.cross_percent, 0, 100) &&
        args.number<std::uint32_t>("--depth", options.depth, 1, kMaxDepth - 1) &&
        args.number<std::uint32_t>("--symbols", options.symbols, 1, core::kMaxSymbols - 1) &&
        args.number<std::int32_t>("--band-low", options.band_low, -1'000'000'000, 1'000'000'000) &&
        args.number<std::uint32_t>("--band-ticks", options.band_ticks, 64, 1U << 24) &&
        args.number<std::uint32_t>("--client-id", options.client_id, 1, 4'000'000'000U) &&
        args.number<std::size_t>("--cpu", options.cpu, 0, sys::kMaxCpus - 1) && args.finish();
    if (good && options.band_ticks < (4 * kSpread)) {
        std::fprintf(stderr, "--band-ticks: too narrow to rest orders in\n");
        return false;
    }
    return good;
}

int run(int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--help" || std::string_view{argv[i]} == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        }
    }
    Options options;
    if (!parse(argc, argv, options)) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    if (options.cpu != sys::kMaxCpus) {
        sys::HotThreadReport report;
        const sys::HotThreadConfig thread{.cpu = options.cpu, .name = "planck-loadgen"};
        if (!ok(sys::configure_hot_thread(thread, report))) {
            std::fprintf(stderr, "planck-loadgen: cannot pin to cpu %zu\n", options.cpu);
            return 1;
        }
    }
    const auto watch = std::make_unique<Stopwatch>();
    if (!ok(watch->calibrate())) {
        std::fprintf(stderr, "planck-loadgen: cannot calibrate the clock\n");
        return 1;
    }
    planck::bench::print_environment(*watch);

    const auto generator = std::make_unique<LoadGenerator>(options, *watch);
    if (!generator->connect()) {
        return 1;
    }
    const bool good = generator->run();
    generator->print();
    return good ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return run(argc, argv);
}
