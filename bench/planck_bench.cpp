// planck-bench: how long the pieces of the engine take, measured in process.
//
// Every benchmark times single operations and keeps all of them in a
// histogram, because the figure that matters for a matching engine is the
// slow case, not the average. See docs/benchmarks.md for how to read the
// output and how to get numbers worth trusting.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "flow.hpp"
#include "harness.hpp"
#include "planck/book/order_book.hpp"
#include "planck/core/hint.hpp"
#include "planck/engine/shard.hpp"
#include "planck/mem/arena.hpp"
#include "planck/perf/histogram.hpp"
#include "planck/ring/spsc.hpp"
#include "planck/sys/cpu.hpp"
#include "planck/wire/codec.hpp"
#include "support.hpp"

namespace {

using planck::bench::keep;
using planck::bench::Rng;
using planck::bench::Stopwatch;
using planck::bench::Table;
using planck::core::ok;
using planck::core::Side;
using planck::core::Status;
using planck::core::TimeInForce;
using planck::perf::Histogram;
namespace book = planck::book;
namespace core = planck::core;
namespace engine = planck::engine;
namespace mem = planck::mem;
namespace ring = planck::ring;
namespace sys = planck::sys;
namespace wire = planck::wire;

constexpr char kUsage[] =
    "planck-bench: time the pieces of the engine\n"
    "\n"
    "  --list            name the benchmarks and stop\n"
    "  --filter TEXT     run only benchmarks whose name contains TEXT\n"
    "  --samples N       operations timed per benchmark       (1000000)\n"
    "  --cpu N           pin the measuring thread to this CPU\n"
    "  --cpu2 N          pin the second thread of the two-thread benchmarks,\n"
    "                    and let it spin instead of yielding\n"
    "  --csv             comma separated output\n"
    "  --quick           a few thousand samples: checks that everything runs\n";

struct Context {
    const Stopwatch& watch;
    const Table& table;
    std::uint64_t samples = 0;
    bool second_cpu_given = false;
    std::size_t second_cpu = 0;
};

// False means the benchmark could not be set up or saw a wrong answer.
using BenchFn = bool (*)(const Context&);

bool complain(std::string_view what) noexcept {
    std::fprintf(stderr, "planck-bench: %.*s\n", static_cast<int>(what.size()), what.data());
    return false;
}

// ---- the timer itself ----

bool bench_timer(const Context& context) {
    // Not adjusted: this is the cost that every other row has had taken off.
    context.table.row("timer.empty_interval", context.watch.overhead());
    return true;
}

// ---- order book ----

struct CountingSink {
    std::uint64_t fills = 0;
    std::uint64_t quantity = 0;
    void on_trade(const book::Trade& trade) noexcept {
        ++fills;
        quantity += trade.qty.raw();
    }
};

constexpr std::uint32_t kBookBand = 1U << 12;
constexpr std::int32_t kMid = static_cast<std::int32_t>(kBookBand / 2);
// How far from the middle resting orders are spread, in ticks.
constexpr std::uint32_t kSpread = 64;

struct BookFixture {
    mem::Arena arena;
    book::OrderStore store;
    book::OrderBook book;
    CountingSink sink;

    [[nodiscard]] bool init(std::uint32_t capacity) noexcept {
        const std::size_t bytes = (capacity * sizeof(book::Order)) +
                                  (kBookBand * sizeof(book::Level)) + (std::size_t{4} << 20);
        if (!ok(arena.init(bytes, sys::HugePages::Try)) ||
            !ok(store.init(arena.allocate_array<book::Order>(capacity), core::ShardId{0})) ||
            !ok(book.init({core::SymbolId{1}, core::Price{0}, kBookBand}, store, arena))) {
            return false;
        }
        arena.prefault();
        return true;
    }

    // An order that will rest: bids below the middle, asks above it.
    [[nodiscard]] static book::NewOrder passive(Rng& rng, std::uint32_t qty) noexcept {
        const bool buy = (rng.next() & 1U) != 0;
        const auto away = static_cast<std::int32_t>(1 + rng.below(kSpread));
        return {.side = buy ? Side::Buy : Side::Sell,
                .type = core::OrderType::Limit,
                .tif = TimeInForce::Day,
                .price = core::Price{buy ? (kMid - away) : (kMid + away)},
                .qty = core::Qty{qty},
                .owner = 1};
    }
};

// A book holding a steady 16,384 orders. Each round cancels one at random
// and enters a new one, so both are timed against a book of realistic depth
// whose orders are scattered through memory.
bool bench_book_add_cancel(const Context& context) {
    constexpr std::uint32_t kDepth = 1U << 14;
    const auto fixture = std::make_unique<BookFixture>();
    if (!fixture->init(kDepth * 2)) {
        return complain("book: no memory");
    }
    Rng rng(7);
    std::vector<core::OrderId> ids(kDepth);
    for (core::OrderId& id : ids) {
        id = fixture->book.add(BookFixture::passive(rng, 10), fixture->sink).id;
    }

    Histogram add;
    Histogram cancel;
    for (std::uint64_t i = 0; i < context.samples; ++i) {
        const std::uint32_t victim = rng.below(kDepth);
        const book::NewOrder order = BookFixture::passive(rng, 10);

        std::uint64_t started = Stopwatch::start();
        const book::CancelResult cancelled = fixture->book.cancel(ids[victim]);
        cancel.record(context.watch.stop(started));

        started = Stopwatch::start();
        const book::AddResult added = fixture->book.add(order, fixture->sink);
        add.record(context.watch.stop(started));

        if (cancelled.reject != book::RejectReason::None ||
            added.reject != book::RejectReason::None || added.resting != order.qty) {
            return complain("book: an add or a cancel went wrong");
        }
        ids[victim] = added.id;
    }
    keep(fixture->sink.fills);
    context.table.row("book.add", add);
    context.table.row("book.cancel", cancel);
    return true;
}

// An incoming buy that fills `fills` resting sell orders in full, against a
// book that is topped up again, untimed, after every round.
bool bench_book_match(const Context& context, std::uint32_t fills, std::string_view name) {
    constexpr std::uint32_t kDepth = 1U << 12;
    constexpr std::uint32_t kLot = 10;
    const auto fixture = std::make_unique<BookFixture>();
    if (!fixture->init(kDepth * 4)) {
        return complain("book: no memory");
    }
    Rng rng(11);
    const auto rest_ask = [&] {
        const auto away = static_cast<std::int32_t>(1 + rng.below(8));
        return fixture->book
            .add({Side::Sell, core::OrderType::Limit, TimeInForce::Day, core::Price{kMid + away},
                  core::Qty{kLot}, 1},
                 fixture->sink)
            .reject == book::RejectReason::None;
    };
    for (std::uint32_t i = 0; i < kDepth; ++i) {
        if (!rest_ask()) {
            return complain("book: could not fill the book");
        }
    }

    const book::NewOrder taker{Side::Buy, core::OrderType::Limit, TimeInForce::ImmediateOrCancel,
                               core::Price{kMid + 64}, core::Qty{kLot * fills}, 2};
    Histogram match;
    for (std::uint64_t i = 0; i < context.samples; ++i) {
        const std::uint64_t before = fixture->sink.fills;
        const std::uint64_t started = Stopwatch::start();
        const book::AddResult result = fixture->book.add(taker, fixture->sink);
        match.record(context.watch.stop(started));
        if (result.filled != taker.qty || fixture->sink.fills != before + fills) {
            return complain("book: a match filled the wrong amount");
        }
        for (std::uint32_t k = 0; k < fills; ++k) {
            if (!rest_ask()) {
                return complain("book: could not top the book up");
            }
        }
    }
    context.table.row(name, match);
    return true;
}

bool bench_book_match_1(const Context& context) {
    return bench_book_match(context, 1, "book.match_1_fill");
}
bool bench_book_match_10(const Context& context) {
    return bench_book_match(context, 10, "book.match_10_fills");
}

// ---- ring ----

bool bench_ring_same_thread(const Context& context) {
    std::vector<engine::Command> storage(1U << 10);
    ring::SpscRing<engine::Command> queue;
    if (!ok(queue.init(storage))) {
        return complain("ring: init failed");
    }
    engine::Command in{};
    engine::Command out{};
    Histogram round_trip;
    for (std::uint64_t i = 0; i < context.samples; ++i) {
        in.id = i;
        const std::uint64_t started = Stopwatch::start();
        const bool pushed = queue.try_push(in);
        const bool popped = queue.try_pop(out);
        round_trip.record(context.watch.stop(started));
        if (!pushed || !popped || out.id != i) {
            return complain("ring: lost a message");
        }
    }
    context.table.row("ring.push_pop", round_trip);
    return true;
}

// The same size as a Command, so the copy costs the same.
struct Stamped {
    std::uint64_t sent_tsc = 0;
    std::uint64_t sequence = 0;
    std::uint64_t padding[2] = {0, 0};
};
static_assert(sizeof(Stamped) == sizeof(engine::Command));

// One thread pushes a time-stamped message every few microseconds. Another
// pops it and records how long ago it was stamped. With the ring empty each
// time, that is the cost of handing one message from core to core.
bool bench_ring_cross_thread(const Context& context) {
    constexpr std::uint64_t kGapNanos = 5'000;
    constexpr std::uint64_t kStop = ~std::uint64_t{0};

    std::vector<Stamped> storage(1U << 10);
    ring::SpscRing<Stamped> queue;
    if (!ok(queue.init(storage))) {
        return complain("ring: init failed");
    }
    const std::uint64_t samples = std::min<std::uint64_t>(context.samples, 200'000);
    const auto latency = std::make_unique<Histogram>();
    std::atomic<bool> ready{false};
    std::atomic<bool> in_order{true};

    std::thread consumer([&] {
        if (context.second_cpu_given) {
            static_cast<void>(sys::pin_current_thread(context.second_cpu));
        }
        ready.store(true, std::memory_order_release);
        std::uint64_t expected = 0;
        Stamped message;
        for (;;) {
            if (!queue.try_pop(message)) {
                core::cpu_relax();
                continue;
            }
            const std::uint64_t now = sys::tsc_now();
            if (message.sequence == kStop) {
                return;
            }
            latency->record(context.watch.adjust(now - message.sent_tsc));
            if (message.sequence != expected) {
                in_order.store(false, std::memory_order_relaxed);
            }
            ++expected;
        }
    });
    while (!ready.load(std::memory_order_acquire)) {
        core::cpu_relax();
    }

    const std::uint64_t gap = context.watch.from_nanos(kGapNanos);
    const auto push = [&](std::uint64_t sequence) {
        Stamped message;
        message.sequence = sequence;
        message.sent_tsc = sys::tsc_now();
        while (!queue.try_push(message)) {
            core::cpu_relax();
            message.sent_tsc = sys::tsc_now();
        }
        return message.sent_tsc;
    };
    for (std::uint64_t i = 0; i < samples; ++i) {
        const std::uint64_t sent = push(i);
        while (sys::tsc_now() - sent < gap) {
            core::cpu_relax();
        }
    }
    static_cast<void>(push(kStop));
    consumer.join();

    if (!in_order.load() || latency->count() != samples) {
        return complain("ring: messages lost or out of order between threads");
    }
    context.table.row("ring.core_to_core", *latency);
    return true;
}

// ---- wire ----

// What the gateway does to every request: find the frame, copy it out,
// check every field, build the command.
bool bench_codec(const Context& context) {
    constexpr std::size_t kMessages = 64;
    std::vector<std::byte> buffer(kMessages * sizeof(wire::NewOrder));
    Rng rng(3);
    for (std::size_t i = 0; i < kMessages; ++i) {
        wire::NewOrder order{};
        order.symbol_id = static_cast<std::uint16_t>(1 + rng.below(8));
        order.side = static_cast<std::uint8_t>(rng.below(2));
        order.tif = static_cast<std::uint8_t>(rng.below(3));
        order.price = static_cast<std::int32_t>(rng.below(4096));
        order.qty = 1 + rng.below(1000);
        order.client_order_id = i + 1;
        std::memcpy(buffer.data() + (i * sizeof(order)), &order, sizeof(order));
    }

    Histogram decode;
    std::uint64_t checksum = 0;
    for (std::uint64_t i = 0; i < context.samples; ++i) {
        const std::size_t offset = (i % kMessages) * sizeof(wire::NewOrder);
        const std::span<const std::byte> bytes{buffer.data() + offset, buffer.size() - offset};
        const std::uint64_t started = Stopwatch::start();
        const wire::Frame frame = wire::next_frame(bytes);
        wire::NewOrder order;
        const bool good = ok(frame.status) && ok(wire::decode(frame.bytes, order)) &&
                          wire::well_formed(order);
        const engine::Command command =
            good ? engine::make_command(order, core::SessionId{1}, started) : engine::Command{};
        keep(command);
        decode.record(context.watch.stop(started));
        if (!good) {
            return complain("codec: a valid message was refused");
        }
        checksum += command.id;
    }
    keep(checksum);
    context.table.row("wire.decode_new_order", decode);
    return true;
}

// ---- shard ----

constexpr std::uint16_t kSymbol = 1;
constexpr std::uint32_t kShardBand = 1U << 16;
constexpr std::int32_t kShardMid = static_cast<std::int32_t>(kShardBand / 2);
constexpr engine::SymbolConfig kShardSymbols[] = {{core::SymbolId{kSymbol}, core::Price{0}, kShardBand}};

engine::ShardConfig shard_config(bool journaling) noexcept {
    engine::ShardConfig config;
    config.shard = core::ShardId{0};
    config.symbols = kShardSymbols;
    config.order_capacity = 1U << 16;
    config.command_ring_capacity = 1U << 12;
    config.report_ring_capacity = 1U << 12;
    config.market_ring_capacity = 1U << 12;
    config.journal_ring_capacity = journaling ? (1U << 12) : 0;
    config.pin = false;
    return config;
}

engine::Command new_order(Side side, std::int32_t price, std::uint32_t qty, std::uint64_t client_id,
                          TimeInForce tif, std::uint16_t session) noexcept {
    wire::NewOrder order{};
    order.symbol_id = kSymbol;
    order.side = std::to_underlying(side);
    order.order_type = std::to_underlying(core::OrderType::Limit);
    order.tif = std::to_underlying(tif);
    order.price = price;
    order.qty = qty;
    order.client_order_id = client_id;
    return engine::make_command(order, core::SessionId{session}, 0);
}

engine::Command passive_order(Rng& rng, std::uint64_t client_id) noexcept {
    const bool buy = (rng.next() & 1U) != 0;
    // Two ticks clear of the middle, where the trade benchmark works.
    const auto away = static_cast<std::int32_t>(2 + rng.below(kSpread));
    return new_order(buy ? Side::Buy : Side::Sell, buy ? (kShardMid - away) : (kShardMid + away),
                     10, client_id, TimeInForce::Day, 1);
}

engine::Command cancel_order(std::uint64_t order_id) noexcept {
    wire::CancelOrder cancel{};
    cancel.order_id = order_id;
    return engine::make_command(cancel, core::SessionId{1}, 0);
}

// What one command produced.
struct Outcome {
    std::size_t reports = 0;
    std::size_t market = 0;
    // From the OrderAccepted, if there was one.
    std::uint64_t order_id = 0;
    // Cycle counter reading the first report carried back.
    std::uint64_t first_ingress_tsc = 0;
};

void drain(engine::Shard& shard, Outcome& outcome) noexcept {
    shard.reports().drain(1U << 12, [&](const engine::Report& report) noexcept {
        if (outcome.reports == 0) {
            outcome.first_ingress_tsc = report.ingress_tsc;
        }
        ++outcome.reports;
        wire::OrderAccepted accepted;
        if (ok(wire::decode({report.bytes.data(), report.length}, accepted))) {
            outcome.order_id = accepted.order_id;
        }
    });
    outcome.market += shard.market().drain(1U << 12, [](const engine::MarketEvent&) noexcept {});
    if (shard.journaling()) {
        static_cast<void>(shard.journal().drain(1U << 12, [](const engine::JournalEntry&) noexcept {}));
    }
}

// A shard driven on the measuring thread: push one command, poll once, and
// time the two together. That covers everything the matching thread does
// for a command: take it off the ring, read the clock, match, encode the
// replies and put them on the outgoing rings. With journaling on it also
// covers handing the command to the journal ring, which is the whole cost
// journaling adds to the matching thread.
bool bench_shard_inline(const Context& context, bool journaling) {
    constexpr std::uint32_t kDepth = 1U << 13;
    const auto shard = std::make_unique<engine::Shard>();
    if (!ok(shard->init(shard_config(journaling)))) {
        return complain("shard: init failed");
    }
    Rng rng(5);
    std::uint64_t client_id = 1;

    // Returns false if the command was not handled at once.
    const auto timed = [&](const engine::Command& command, Histogram& histogram, Outcome& outcome) {
        const std::uint64_t started = Stopwatch::start();
        const bool pushed = shard->commands().try_push(command);
        const std::size_t handled = shard->poll(1);
        histogram.record(context.watch.stop(started));
        drain(*shard, outcome);
        return pushed && handled == 1;
    };
    const auto untimed = [&](const engine::Command& command, Outcome& outcome) {
        const bool pushed = shard->commands().try_push(command);
        const std::size_t handled = shard->poll(1);
        drain(*shard, outcome);
        return pushed && handled == 1;
    };

    std::vector<std::uint64_t> ids(kDepth);
    for (std::uint64_t& id : ids) {
        Outcome outcome;
        if (!untimed(passive_order(rng, client_id++), outcome) || outcome.order_id == 0) {
            return complain("shard: could not fill the book");
        }
        id = outcome.order_id;
    }

    Histogram rest;
    Histogram cancel;
    Histogram trade;
    for (std::uint64_t i = 0; i < context.samples; ++i) {
        const std::uint32_t victim = rng.below(kDepth);

        Outcome cancelled;
        if (!timed(cancel_order(ids[victim]), cancel, cancelled) || cancelled.reports != 1 ||
            cancelled.market != 1) {
            return complain("shard: a cancel went wrong");
        }
        Outcome rested;
        if (!timed(passive_order(rng, client_id++), rest, rested) || rested.order_id == 0 ||
            rested.market != 1) {
            return complain("shard: an order did not rest");
        }
        ids[victim] = rested.order_id;

        // One order resting alone at the middle, then the order that takes
        // it: an acceptance, a fill to each side and one trade print.
        Outcome maker;
        Outcome taker;
        if (!untimed(new_order(Side::Sell, kShardMid, 10, client_id++, TimeInForce::Day, 1), maker) ||
            !timed(new_order(Side::Buy, kShardMid, 10, client_id++, TimeInForce::ImmediateOrCancel, 2),
                   trade, taker) ||
            taker.reports != 3 || taker.market != 1) {
            return complain("shard: a trade went wrong");
        }
    }

    const std::string_view suffix = journaling ? ".journal" : "";
    char name[40];
    const auto row = [&](std::string_view what, const Histogram& histogram) {
        const int length = std::snprintf(name, sizeof(name), "shard.%.*s%.*s",
                                         static_cast<int>(what.size()), what.data(),
                                         static_cast<int>(suffix.size()), suffix.data());
        context.table.row({name, static_cast<std::size_t>(length)}, histogram);
    };
    row("rest", rest);
    row("cancel", cancel);
    row("trade", trade);
    return true;
}

bool bench_shard(const Context& context) { return bench_shard_inline(context, false); }
bool bench_shard_journal(const Context& context) { return bench_shard_inline(context, true); }

// A shard on a thread of its own, as in the venue. The measuring thread
// plays the gateway: it stamps a command, pushes it, and spins on the report
// ring until the answer is there. One command is in flight at a time, so
// nothing ever queues. The figure is ring in to ring out across two cores.
bool bench_shard_thread(const Context& context) {
    constexpr std::uint32_t kDepth = 1U << 13;
    constexpr std::uint64_t kPatienceNanos = 5'000'000'000ULL;
    const auto shard = std::make_unique<engine::Shard>();
    std::atomic<int> state{0};  // 0 starting, 1 running, -1 failed
    std::atomic<bool> stop{false};

    std::thread thread([&] {
        engine::ShardConfig config = shard_config(false);
        config.pin = context.second_cpu_given;
        config.cpu = context.second_cpu;
        config.idle = context.second_cpu_given ? engine::IdlePolicy::Spin : engine::IdlePolicy::Yield;
        if (!ok(shard->init(config))) {
            state.store(-1, std::memory_order_release);
            return;
        }
        state.store(1, std::memory_order_release);
        shard->run(stop);
    });
    while (state.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    // Whatever happens below, the thread is stopped and joined on the way out.
    const auto finish = [&](bool good, std::string_view why) {
        stop.store(true, std::memory_order_relaxed);
        thread.join();
        return good ? true : complain(why);
    };
    if (state.load() < 0) {
        return finish(false, "shard thread: init failed");
    }

    const std::uint64_t patience = context.watch.from_nanos(kPatienceNanos);
    // Send one command and wait for `expected` reports. The latency of the
    // first one goes into `histogram` if there is one.
    const auto exchange = [&](engine::Command command, std::size_t expected, Histogram* histogram,
                              Outcome& outcome) {
        command.ingress_tsc = sys::tsc_now();
        if (!shard->commands().try_push(command)) {
            return false;
        }
        while (outcome.reports < expected) {
            const std::size_t before = outcome.reports;
            drain(*shard, outcome);
            const std::uint64_t now = sys::tsc_now();
            if (before == 0 && outcome.reports != 0 && histogram != nullptr) {
                histogram->record(context.watch.adjust(now - outcome.first_ingress_tsc));
            }
            if (now - command.ingress_tsc > patience) {
                return false;
            }
            if (outcome.reports == before) {
                core::cpu_relax();
            }
        }
        return outcome.first_ingress_tsc == command.ingress_tsc;
    };

    Rng rng(9);
    std::uint64_t client_id = 1;
    std::vector<std::uint64_t> ids(kDepth);
    for (std::uint64_t& id : ids) {
        Outcome outcome;
        if (!exchange(passive_order(rng, client_id++), 1, nullptr, outcome) || outcome.order_id == 0) {
            return finish(false, "shard thread: could not fill the book");
        }
        id = outcome.order_id;
    }

    const std::uint64_t samples = std::min<std::uint64_t>(context.samples, 300'000);
    const auto rest = std::make_unique<Histogram>();
    const auto cancel = std::make_unique<Histogram>();
    const auto trade = std::make_unique<Histogram>();
    for (std::uint64_t i = 0; i < samples; ++i) {
        const std::uint32_t victim = rng.below(kDepth);
        Outcome cancelled;
        Outcome rested;
        Outcome maker;
        Outcome taker;
        if (!exchange(cancel_order(ids[victim]), 1, cancel.get(), cancelled) ||
            !exchange(passive_order(rng, client_id++), 1, rest.get(), rested) ||
            rested.order_id == 0 ||
            !exchange(new_order(Side::Sell, kShardMid, 10, client_id++, TimeInForce::Day, 1), 1,
                      nullptr, maker) ||
            !exchange(new_order(Side::Buy, kShardMid, 10, client_id++, TimeInForce::ImmediateOrCancel, 2),
                      3, trade.get(), taker)) {
            return finish(false, "shard thread: an exchange went wrong or timed out");
        }
        ids[victim] = rested.order_id;
    }
    static_cast<void>(finish(true, ""));
    context.table.row("shard_thread.rest", *rest);
    context.table.row("shard_thread.cancel", *cancel);
    context.table.row("shard_thread.trade", *trade);
    return true;
}

struct Bench {
    std::string_view name;
    std::string_view what;
    BenchFn run;
};

constexpr Bench kBenches[] = {
    {"timer", "an empty timed interval: the cost taken off every other row", bench_timer},
    {"book.add_cancel", "book.add and book.cancel on a book 16,384 orders deep", bench_book_add_cancel},
    {"book.match_1", "an incoming order that fills one resting order", bench_book_match_1},
    {"book.match_10", "an incoming order that fills ten resting orders", bench_book_match_10},
    {"ring.same_thread", "one push and one pop of a 32-byte command", bench_ring_same_thread},
    {"ring.cross_thread", "one message handed from one core to another", bench_ring_cross_thread},
    {"wire.decode", "frame, copy, validate and convert one NewOrder", bench_codec},
    {"shard.inline", "a whole command through a shard: rest, cancel, trade", bench_shard},
    {"shard.journal", "the same with journaling on", bench_shard_journal},
    {"shard.thread", "command in to report out across two threads", bench_shard_thread},
};

int run(int argc, const char* const* argv) {
    planck::apps::Args args(argc, argv);
    if (args.flag("--help") || args.flag("-h")) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    const bool list = args.flag("--list");
    const bool csv = args.flag("--csv");
    const bool quick = args.flag("--quick");
    std::string_view filter;
    std::uint64_t samples = quick ? 3'000 : 1'000'000;
    std::size_t cpu = sys::kMaxCpus;
    std::size_t cpu2 = sys::kMaxCpus;
    if (!args.text("--filter", filter) ||
        !args.number<std::uint64_t>("--samples", samples, 100, 1'000'000'000) ||
        !args.number<std::size_t>("--cpu", cpu, 0, sys::kMaxCpus - 1) ||
        !args.number<std::size_t>("--cpu2", cpu2, 0, sys::kMaxCpus - 1) || !args.finish()) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    if (list) {
        for (const Bench& bench : kBenches) {
            std::printf("%-18.*s %.*s\n", static_cast<int>(bench.name.size()), bench.name.data(),
                        static_cast<int>(bench.what.size()), bench.what.data());
        }
        return 0;
    }

    if (cpu != sys::kMaxCpus) {
        sys::HotThreadReport report;
        const sys::HotThreadConfig thread{.cpu = cpu, .name = "planck-bench"};
        if (!ok(sys::configure_hot_thread(thread, report))) {
            std::fprintf(stderr, "planck-bench: cannot pin to cpu %zu\n", cpu);
            return 1;
        }
    }
    // After pinning, so the clock is calibrated on the CPU that will use it.
    const auto watch = std::make_unique<Stopwatch>();
    if (!ok(watch->calibrate())) {
        std::fprintf(stderr, "planck-bench: cannot calibrate the clock\n");
        return 1;
    }
    planck::bench::print_environment(*watch);

    const Table table(*watch, csv);
    const Context context{.watch = *watch,
                          .table = table,
                          .samples = samples,
                          .second_cpu_given = (cpu2 != sys::kMaxCpus),
                          .second_cpu = (cpu2 != sys::kMaxCpus) ? cpu2 : 0};
    table.header();
    std::size_t ran = 0;
    bool all_good = true;
    for (const Bench& bench : kBenches) {
        if (bench.name.find(filter) == std::string_view::npos) {
            continue;
        }
        ++ran;
        all_good = bench.run(context) && all_good;
        std::fflush(stdout);
    }
    if (ran == 0) {
        std::fprintf(stderr, "planck-bench: no benchmark matches '%.*s'. Try --list.\n",
                     static_cast<int>(filter.size()), filter.data());
        return 2;
    }
    return all_good ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return run(argc, argv);
}
