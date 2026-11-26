// planckd: the venue. One matching thread per shard, one order entry gateway
// and one market data publisher, connected only by rings.
//
//   clients --TCP--> gateway --> shards --> gateway --TCP--> clients
//                                   \--> publisher --UDP--> subscribers

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>

#include <sched.h>
#include <time.h>

#include "planck/core/bits.hpp"
#include "planck/core/hint.hpp"
#include "planck/engine/shard.hpp"
#include "planck/feed/publisher.hpp"
#include "planck/gateway/gateway.hpp"
#include "planck/sys/cpu.hpp"
#include "planck/sys/memory.hpp"
#include "support.hpp"

namespace {

using planck::core::ok;
using planck::core::Status;
namespace apps = planck::apps;
namespace core = planck::core;
namespace engine = planck::engine;
namespace feed = planck::feed;
namespace gateway = planck::gateway;
namespace net = planck::net;
namespace sys = planck::sys;

constexpr char kUsage[] =
    "planckd: a limit order matching venue\n"
    "\n"
    "  --listen ADDRESS:PORT      order entry, TCP            (0.0.0.0:9000)\n"
    "  --feed ADDRESS:PORT        market data, UDP. Shard N   (239.255.42.1:30001)\n"
    "                             sends to PORT + N. A multicast group or a plain address.\n"
    "  --retransmit ADDRESS:PORT  feed retransmit requests    (0.0.0.0:30000)\n"
    "  --shards N                 matching threads, 1 to 16   (1)\n"
    "  --symbols N                symbols traded, ids 1 to N  (8)\n"
    "                             Symbol S trades on shard S mod N.\n"
    "  --band-low TICKS           lowest price accepted       (0)\n"
    "  --band-ticks N             prices per symbol, a power of two (65536)\n"
    "  --orders N                 resting orders per shard    (1048576)\n"
    "  --cpus LIST                pin threads, e.g. 2-5. Shards take the first CPUs,\n"
    "                             then the gateway, then the publisher.\n"
    "  --idle spin|yield|sleep    what an idle thread does    (spin with --cpus, else sleep)\n"
    "  --huge off|try|require     2 MiB pages                 (try)\n"
    "  --lock-memory              keep every page in RAM\n"
    "  --stats SECONDS            print counters this often, 0 for never (0)\n"
    "\n"
    "Stop it with Ctrl-C or SIGTERM.\n";

enum class Idle : std::uint8_t { Spin, Yield, Sleep };

struct Options {
    net::Endpoint listen{net::kAnyAddress, apps::kDefaultOrderPort};
    net::Endpoint feed{apps::kDefaultFeedGroup, apps::kDefaultFeedPort};
    net::Endpoint retransmit{net::kAnyAddress, apps::kDefaultRetransmitPort};
    std::uint32_t shards = 1;
    std::uint32_t symbols = 8;
    std::int32_t band_low = 0;
    std::uint32_t band_ticks = 1U << 16;
    std::uint32_t orders = 1U << 20;
    std::uint32_t stats_seconds = 0;
    sys::HugePages huge = sys::HugePages::Try;
    Idle idle = Idle::Sleep;
    bool lock_memory = false;
    bool pin = false;
    // One CPU per thread when pinning: shards, then gateway, then publisher.
    std::array<std::size_t, core::kMaxShards + 2> cpus{};
};

// A counter one thread writes and another reads. The components keep plain
// counters for speed. Their own thread copies them here now and then, so
// the main thread can print them without racing.
struct Counter {
    std::atomic<std::uint64_t> value{0};
    void set(std::uint64_t v) noexcept { value.store(v, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t get() const noexcept {
        return value.load(std::memory_order_relaxed);
    }
};

enum class State : int { Starting = 0, Running = 1, Failed = -1 };

// What every thread reports back to main.
struct Lifecycle {
    std::atomic<State> state{State::Starting};
    // Valid once state is Failed.
    Status error = Status::Ok;
};

struct ShardRuntime {
    engine::Shard shard;
    Lifecycle life;
    std::thread thread;
    Counter commands, accepted, rejected, trades, cancels, cancel_rejects, stalls, resting;

    void publish_stats() noexcept {
        const engine::ShardStats& s = shard.stats();
        commands.set(s.commands);
        accepted.set(s.accepted);
        rejected.set(s.rejected);
        trades.set(s.trades);
        cancels.set(s.cancels);
        cancel_rejects.set(s.cancel_rejects);
        stalls.set(s.stalls);
        resting.set(shard.store().in_use());
    }
};

struct GatewayRuntime {
    gateway::Gateway gateway;
    Lifecycle life;
    std::thread thread;
    // Valid once state is Running.
    net::Endpoint endpoint{};
    Counter sessions, connections, logins, messages_in, rejected, busy, reports_out,
        reports_dropped, protocol_errors, slow_consumers;

    void publish_stats() noexcept {
        const gateway::GatewayStats& s = gateway.stats();
        sessions.set(gateway.active_sessions());
        connections.set(s.connections);
        logins.set(s.logins);
        messages_in.set(s.messages_in);
        rejected.set(s.rejected);
        busy.set(s.busy);
        reports_out.set(s.reports_out);
        reports_dropped.set(s.reports_dropped);
        protocol_errors.set(s.protocol_errors);
        slow_consumers.set(s.slow_consumers);
    }
};

struct PublisherRuntime {
    feed::Publisher publisher;
    Lifecycle life;
    std::thread thread;
    net::Endpoint endpoint{};
    Counter messages, datagrams, heartbeats, send_failures, retransmit_requests,
        retransmitted_messages, retransmit_refused;

    void publish_stats() noexcept {
        const feed::PublisherStats& s = publisher.stats();
        messages.set(s.messages);
        datagrams.set(s.datagrams);
        heartbeats.set(s.heartbeats);
        send_failures.set(s.send_failures);
        retransmit_requests.set(s.retransmit_requests);
        retransmitted_messages.set(s.retransmitted_messages);
        retransmit_refused.set(s.retransmit_refused);
    }
};

// Everything the process owns. Built once at startup and never moved: the
// threads hold pointers into it.
struct Venue {
    std::array<ShardRuntime, core::kMaxShards> shards;
    GatewayRuntime gateway;
    PublisherRuntime publisher;

    std::array<engine::Shard*, core::kMaxShards> shard_pointers{};
    std::array<engine::SymbolConfig, core::kMaxSymbols> symbols{};
    std::array<gateway::Route, core::kMaxSymbols> routes{};
    std::array<net::Endpoint, core::kMaxShards> destinations{};

    // Matching stops first. The gateway and the publisher keep running until
    // the shards have gone, so nothing a shard produced is left behind.
    std::atomic<bool> stop_shards{false};
    std::atomic<bool> stop_io{false};
};

void idle_once(Idle idle) noexcept {
    switch (idle) {
        case Idle::Spin:
            core::cpu_relax();
            break;
        case Idle::Yield:
            static_cast<void>(::sched_yield());
            break;
        case Idle::Sleep: {
            const timespec nap{.tv_sec = 0, .tv_nsec = 50'000};
            static_cast<void>(::nanosleep(&nap, nullptr));
            break;
        }
    }
}

// The body of every thread once its component is up: poll until told to
// stop, then poll a little longer to empty what is still in flight.
template <typename Runtime, typename Poll>
void drive(Runtime& runtime, const std::atomic<bool>& stop, Idle idle, Poll poll) noexcept {
    constexpr std::uint64_t kStatsEvery = 1024;
    constexpr int kMaxDrainPasses = 100'000;

    std::uint64_t passes = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        if (poll() == 0) {
            idle_once(idle);
        }
        ++passes;
        if ((passes % kStatsEvery) == 0) {
            runtime.publish_stats();
        }
    }
    for (int pass = 0; pass < kMaxDrainPasses && poll() != 0; ++pass) {
    }
    runtime.publish_stats();
}

void fail(Lifecycle& life, Status status) noexcept {
    life.error = status;
    life.state.store(State::Failed);
}

void shard_main(Venue& venue, const Options& options, std::size_t index,
                std::span<const engine::SymbolConfig> symbols) noexcept {
    ShardRuntime& runtime = venue.shards[index];
    engine::ShardConfig config;
    config.shard = core::ShardId{static_cast<std::uint8_t>(index)};
    config.symbols = symbols;
    config.order_capacity = options.orders;
    config.huge_pages = options.huge;
    config.pin = options.pin;
    config.cpu = options.cpus[index];
    if (const Status status = runtime.shard.init(config); !ok(status)) {
        fail(runtime.life, status);
        return;
    }
    runtime.life.state.store(State::Running);
    drive(runtime, venue.stop_shards, options.idle,
          [&runtime]() noexcept { return runtime.shard.poll(); });
}

void gateway_main(Venue& venue, const Options& options) noexcept {
    GatewayRuntime& runtime = venue.gateway;
    gateway::GatewayConfig config;
    config.listen = options.listen;
    config.shards = std::span<engine::Shard* const>{venue.shard_pointers.data(), options.shards};
    config.routes = std::span<const gateway::Route>{venue.routes.data(), options.symbols};
    config.pin = options.pin;
    config.cpu = options.cpus[options.shards];
    Status status = runtime.gateway.init(config);
    if (ok(status)) {
        status = runtime.gateway.local_endpoint(runtime.endpoint);
    }
    if (!ok(status)) {
        fail(runtime.life, status);
        return;
    }
    runtime.life.state.store(State::Running);
    drive(runtime, venue.stop_io, options.idle,
          [&runtime]() noexcept { return runtime.gateway.poll(); });
}

void publisher_main(Venue& venue, const Options& options) noexcept {
    PublisherRuntime& runtime = venue.publisher;
    feed::PublisherConfig config;
    config.shards = std::span<engine::Shard* const>{venue.shard_pointers.data(), options.shards};
    config.destinations =
        std::span<const net::Endpoint>{venue.destinations.data(), options.shards};
    config.retransmit_listen = options.retransmit;
    config.pin = options.pin;
    config.cpu = options.cpus[options.shards + 1];
    Status status = runtime.publisher.init(config);
    if (ok(status)) {
        status = runtime.publisher.retransmit_endpoint(runtime.endpoint);
    }
    if (!ok(status)) {
        fail(runtime.life, status);
        return;
    }
    runtime.life.state.store(State::Running);
    drive(runtime, venue.stop_io, options.idle,
          [&runtime]() noexcept { return runtime.publisher.poll(); });
}

// Wait for a thread to finish starting. False if it failed.
[[nodiscard]] bool started(const Lifecycle& life, const char* what) noexcept {
    while (life.state.load() == State::Starting) {
        idle_once(Idle::Sleep);
    }
    if (life.state.load() == State::Failed) {
        const std::string_view reason = core::to_string(life.error);
        std::fprintf(stderr, "planckd: %s failed to start: %.*s\n", what,
                     static_cast<int>(reason.size()), reason.data());
        return false;
    }
    return true;
}

void print_stats(const Venue& venue, std::uint32_t shards) noexcept {
    std::uint64_t commands = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t trades = 0;
    std::uint64_t cancels = 0;
    std::uint64_t resting = 0;
    std::uint64_t stalls = 0;
    for (std::uint32_t i = 0; i < shards; ++i) {
        const ShardRuntime& shard = venue.shards[i];
        commands += shard.commands.get();
        accepted += shard.accepted.get();
        rejected += shard.rejected.get() + shard.cancel_rejects.get();
        trades += shard.trades.get();
        cancels += shard.cancels.get();
        resting += shard.resting.get();
        stalls += shard.stalls.get();
    }
    const GatewayRuntime& g = venue.gateway;
    const PublisherRuntime& p = venue.publisher;
    std::printf("stats engine commands=%llu accepted=%llu rejected=%llu trades=%llu cancels=%llu "
                "resting=%llu stalls=%llu\n",
                static_cast<unsigned long long>(commands),
                static_cast<unsigned long long>(accepted),
                static_cast<unsigned long long>(rejected), static_cast<unsigned long long>(trades),
                static_cast<unsigned long long>(cancels), static_cast<unsigned long long>(resting),
                static_cast<unsigned long long>(stalls));
    std::printf("stats gateway sessions=%llu connections=%llu logins=%llu messages=%llu "
                "rejected=%llu busy=%llu reports=%llu dropped=%llu protocol_errors=%llu "
                "slow_consumers=%llu\n",
                static_cast<unsigned long long>(g.sessions.get()),
                static_cast<unsigned long long>(g.connections.get()),
                static_cast<unsigned long long>(g.logins.get()),
                static_cast<unsigned long long>(g.messages_in.get()),
                static_cast<unsigned long long>(g.rejected.get()),
                static_cast<unsigned long long>(g.busy.get()),
                static_cast<unsigned long long>(g.reports_out.get()),
                static_cast<unsigned long long>(g.reports_dropped.get()),
                static_cast<unsigned long long>(g.protocol_errors.get()),
                static_cast<unsigned long long>(g.slow_consumers.get()));
    std::printf("stats feed messages=%llu datagrams=%llu heartbeats=%llu send_failures=%llu "
                "retransmit_requests=%llu retransmitted=%llu refused=%llu\n",
                static_cast<unsigned long long>(p.messages.get()),
                static_cast<unsigned long long>(p.datagrams.get()),
                static_cast<unsigned long long>(p.heartbeats.get()),
                static_cast<unsigned long long>(p.send_failures.get()),
                static_cast<unsigned long long>(p.retransmit_requests.get()),
                static_cast<unsigned long long>(p.retransmitted_messages.get()),
                static_cast<unsigned long long>(p.retransmit_refused.get()));
}

[[nodiscard]] bool parse_choices(apps::Args& args, Options& options) noexcept {
    std::string_view huge = "try";
    std::string_view idle;
    std::string_view cpus;
    if (!args.text("--huge", huge) || !args.text("--idle", idle) || !args.text("--cpus", cpus)) {
        return false;
    }

    if (huge == "off") {
        options.huge = sys::HugePages::Off;
    } else if (huge == "try") {
        options.huge = sys::HugePages::Try;
    } else if (huge == "require") {
        options.huge = sys::HugePages::Require;
    } else {
        std::fprintf(stderr, "--huge: expected off, try or require\n");
        return false;
    }

    if (!cpus.empty()) {
        sys::CpuSet set;
        if (!ok(sys::parse_cpu_list(cpus, set))) {
            std::fprintf(stderr, "--cpus: expected a list such as 2-5 or 2,4,6,8\n");
            return false;
        }
        const std::size_t needed = options.shards + 2;
        std::size_t found = 0;
        for (std::size_t cpu = 0; cpu < sys::kMaxCpus && found < needed; ++cpu) {
            if (set.test(cpu)) {
                options.cpus[found] = cpu;
                ++found;
            }
        }
        if (found < needed) {
            std::fprintf(stderr, "--cpus: %u shards need %zu CPUs (one each, plus the gateway "
                                 "and the publisher), got %zu\n",
                         options.shards, needed, found);
            return false;
        }
        options.pin = true;
    }

    // A pinned thread has a core to itself, so it may as well spin. An
    // unpinned one is a guest on a shared machine and should be polite.
    options.idle = options.pin ? Idle::Spin : Idle::Sleep;
    if (idle == "spin") {
        options.idle = Idle::Spin;
    } else if (idle == "yield") {
        options.idle = Idle::Yield;
    } else if (idle == "sleep") {
        options.idle = Idle::Sleep;
    } else if (!idle.empty()) {
        std::fprintf(stderr, "--idle: expected spin, yield or sleep\n");
        return false;
    }
    return true;
}

[[nodiscard]] bool parse_options(apps::Args& args, Options& options) noexcept {
    options.lock_memory = args.flag("--lock-memory");
    const bool parsed =
        args.endpoint("--listen", options.listen) && args.endpoint("--feed", options.feed) &&
        args.endpoint("--retransmit", options.retransmit) &&
        args.number<std::uint32_t>("--shards", options.shards, 1, core::kMaxShards) &&
        args.number<std::uint32_t>("--symbols", options.symbols, 1, core::kMaxSymbols - 1) &&
        args.number<std::int32_t>("--band-low", options.band_low, -(1 << 30), 1 << 30) &&
        args.number<std::uint32_t>("--band-ticks", options.band_ticks,
                                   planck::book::OrderBook::kMinBandTicks,
                                   planck::book::OrderBook::kMaxBandTicks) &&
        args.number<std::uint32_t>("--orders", options.orders, 1,
                                   planck::book::OrderStore::kMaxCapacity) &&
        args.number<std::uint32_t>("--stats", options.stats_seconds, 0, 86'400) &&
        parse_choices(args, options) && args.finish();
    if (!parsed) {
        return false;
    }
    if (!core::is_pow2(options.band_ticks)) {
        std::fprintf(stderr, "--band-ticks: must be a power of two\n");
        return false;
    }
    if (options.shards > options.symbols) {
        std::fprintf(stderr, "--shards: %u shards but only %u symbols to trade\n", options.shards,
                     options.symbols);
        return false;
    }
    if (options.feed.port == 0 || (options.feed.port + options.shards - 1) > 65535U) {
        std::fprintf(stderr, "--feed: ports PORT to PORT + shards - 1 must all be valid\n");
        return false;
    }
    return true;
}

// Decide who trades what. Symbol ids run from 1. Symbol S goes to shard
// S mod shards. Each shard's symbols end up next to each other in
// venue.symbols, and counts says how many each shard has.
void assign_symbols(Venue& venue, const Options& options,
                    std::array<std::size_t, core::kMaxShards>& counts) noexcept {
    std::size_t next = 0;
    for (std::uint32_t shard = 0; shard < options.shards; ++shard) {
        counts[shard] = 0;
        for (std::uint32_t id = 1; id <= options.symbols; ++id) {
            if ((id % options.shards) != shard) {
                continue;
            }
            const core::SymbolId symbol{static_cast<std::uint16_t>(id)};
            venue.symbols[next] = engine::SymbolConfig{.symbol = symbol,
                                                       .band_low = core::Price{options.band_low},
                                                       .band_ticks = options.band_ticks};
            venue.routes[next] =
                gateway::Route{.symbol = symbol, .shard_index = static_cast<std::uint8_t>(shard)};
            ++next;
            ++counts[shard];
        }
        venue.shard_pointers[shard] = &venue.shards[shard].shard;
        venue.destinations[shard] = net::Endpoint{
            options.feed.address, static_cast<std::uint16_t>(options.feed.port + shard)};
    }
}

void join(std::thread& thread) noexcept {
    if (thread.joinable()) {
        thread.join();
    }
}

// Stop everything that was started, in the order that loses nothing.
void shut_down(Venue& venue, std::uint32_t shards) noexcept {
    venue.stop_shards.store(true);
    for (std::uint32_t i = 0; i < shards; ++i) {
        join(venue.shards[i].thread);
    }
    venue.stop_io.store(true);
    join(venue.gateway.thread);
    join(venue.publisher.thread);
}

[[nodiscard]] bool start(Venue& venue, const Options& options,
                         const std::array<std::size_t, core::kMaxShards>& counts) noexcept {
    // Each component is initialised on the thread that will run it, so the
    // memory it touches lands near that thread's CPU.
    std::size_t offset = 0;
    for (std::uint32_t i = 0; i < options.shards; ++i) {
        const std::span<const engine::SymbolConfig> symbols{venue.symbols.data() + offset,
                                                            counts[i]};
        offset += counts[i];
        venue.shards[i].thread =
            std::thread([&venue, &options, i, symbols] { shard_main(venue, options, i, symbols); });
    }
    bool up = true;
    for (std::uint32_t i = 0; i < options.shards; ++i) {
        up = started(venue.shards[i].life, "a matching shard") && up;
    }
    if (!up) {
        return false;
    }

    // The gateway and the publisher need the shards to exist first.
    venue.gateway.thread = std::thread([&venue, &options] { gateway_main(venue, options); });
    venue.publisher.thread = std::thread([&venue, &options] { publisher_main(venue, options); });
    const bool gateway_up = started(venue.gateway.life, "the order entry gateway");
    const bool publisher_up = started(venue.publisher.life, "the market data publisher");
    if (!publisher_up && net::is_multicast(options.feed.address)) {
        std::fprintf(stderr, "planckd: if this host cannot send multicast, try "
                             "--feed 127.0.0.1:%u\n",
                     options.feed.port);
    }
    return gateway_up && publisher_up;
}

void print_banner(const Venue& venue, const Options& options) noexcept {
    std::printf("planckd %u shard%s, %u symbol%s (ids 1 to %u), prices %d to %lld\n",
                options.shards, options.shards == 1 ? "" : "s", options.symbols,
                options.symbols == 1 ? "" : "s", options.symbols, options.band_low,
                static_cast<long long>(options.band_low) + options.band_ticks - 1);
    for (std::uint32_t i = 0; i < options.shards; ++i) {
        const sys::HotThreadReport& report = venue.shards[i].shard.thread_report();
        if (options.pin) {
            std::printf("shard %u on cpu %zu%s%s\n", i, options.cpus[i],
                        report.isolated ? ", isolated" : ", not isolated",
                        report.tickless ? ", tickless" : "");
        }
        std::printf("feed stream %u -> %s\n", i,
                    apps::format_endpoint(venue.destinations[i]).c_str());
    }
    std::printf("order entry on %s\n", apps::format_endpoint(venue.gateway.endpoint).c_str());
    std::printf("retransmit requests on %s\n",
                apps::format_endpoint(venue.publisher.endpoint).c_str());
    std::printf("ready\n");
}

int run(const Options& options) noexcept {
    const std::atomic<bool>& stop = apps::stop_on_signal();
    if (options.lock_memory) {
        if (const Status status = sys::lock_all_memory(); !ok(status)) {
            const std::string_view reason = core::to_string(status);
            std::fprintf(stderr, "planckd: --lock-memory failed: %.*s (raise ulimit -l)\n",
                         static_cast<int>(reason.size()), reason.data());
            return 1;
        }
    }

    // Too big for the stack, and it must never move.
    const std::unique_ptr<Venue> venue{new Venue{}};
    std::array<std::size_t, core::kMaxShards> counts{};
    assign_symbols(*venue, options, counts);

    if (!start(*venue, options, counts)) {
        shut_down(*venue, options.shards);
        return 1;
    }
    print_banner(*venue, options);

    const timespec tick{.tv_sec = 0, .tv_nsec = 100'000'000};
    const std::uint32_t ticks_per_report = options.stats_seconds * 10;
    std::uint32_t ticks = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        static_cast<void>(::nanosleep(&tick, nullptr));
        ++ticks;
        if (ticks_per_report != 0 && ticks >= ticks_per_report) {
            print_stats(*venue, options.shards);
            ticks = 0;
        }
    }

    std::printf("stopping\n");
    shut_down(*venue, options.shards);
    print_stats(*venue, options.shards);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Line buffered even when piped to a file, so a log is never half a
    // line behind.
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    apps::Args args(argc, argv);
    if (args.flag("--help")) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    Options options;
    if (!parse_options(args, options)) {
        return 2;
    }
    return run(options);
}
