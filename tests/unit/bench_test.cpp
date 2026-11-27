#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>

#include "flow.hpp"
#include "harness.hpp"
#include "planck/sys/clock.hpp"

namespace planck::bench {
namespace {

TEST(BenchRng, IsRepeatableAndStaysInRange) {
    Rng first(42);
    Rng second(42);
    Rng other(43);
    bool differs = false;
    std::set<std::uint32_t> seen;
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t value = first.next();
        ASSERT_EQ(value, second.next());
        differs = differs || (value != other.next());
        const std::uint32_t small = first.below(7);
        ASSERT_LT(small, 7U);
        ASSERT_EQ(small, second.below(7));
        seen.insert(small);
    }
    EXPECT_TRUE(differs) << "two seeds gave the same sequence";
    EXPECT_EQ(seen.size(), 7U) << "some values below the bound never came up";

    Rng zero(0);
    EXPECT_NE(zero.next(), 0U) << "a zero seed must not get stuck at zero";
}

TEST(BenchPacer, NothingIsDueEarly) {
    Pacer pacer(1000, 100);
    std::uint64_t intended = 7;
    EXPECT_FALSE(pacer.due(999, intended));
    EXPECT_EQ(intended, 7U) << "output touched when nothing was due";
    ASSERT_TRUE(pacer.due(1000, intended));
    EXPECT_EQ(intended, 1000U);
    EXPECT_FALSE(pacer.due(1099, intended));
    ASSERT_TRUE(pacer.due(1100, intended));
    EXPECT_EQ(intended, 1100U);
}

TEST(BenchPacer, AfterAStallEveryMissedSlotIsStillDueAtItsOwnTime) {
    // The sender was stuck from 1000 until 1450. The slots at 1000, 1100,
    // 1200, 1300 and 1400 all fell due in that time. Each must still go out,
    // and each must be measured from when it was due, not from 1450.
    Pacer pacer(1000, 100);
    std::uint64_t intended = 0;
    for (std::uint64_t expected = 1000; expected <= 1400; expected += 100) {
        ASSERT_TRUE(pacer.due(1450, intended));
        EXPECT_EQ(intended, expected);
    }
    EXPECT_FALSE(pacer.due(1450, intended));
    ASSERT_TRUE(pacer.due(1500, intended));
    EXPECT_EQ(intended, 1500U) << "the schedule drifted after a stall";
}

TEST(BenchSendLog, HandsBackWhatWasPutExactlyOnce) {
    SendLog<8> log;
    ASSERT_TRUE(log.put(1, {.sent = 10, .intended = 9, .tag = 100}));
    ASSERT_TRUE(log.put(2, {.sent = 20, .intended = 19, .tag = 200}));
    EXPECT_EQ(log.pending(), 2U);

    SendTimes times;
    ASSERT_TRUE(log.take(2, times));
    EXPECT_EQ(times.sent, 20U);
    EXPECT_EQ(times.intended, 19U);
    EXPECT_EQ(times.tag, 200U);
    EXPECT_FALSE(log.take(2, times)) << "one request answered twice";
    EXPECT_FALSE(log.take(3, times)) << "an answer to a request never sent";
    EXPECT_FALSE(log.take(9, times)) << "9 shares a slot with 1 but is not 1";
    EXPECT_FALSE(log.take(0, times));
    EXPECT_EQ(log.pending(), 1U);
    ASSERT_TRUE(log.take(1, times));
    EXPECT_EQ(times.sent, 10U);
    EXPECT_EQ(log.pending(), 0U);
}

TEST(BenchSendLog, RefusesToOverwriteAnUnansweredRequest) {
    SendLog<4> log;
    for (std::uint64_t id = 1; id <= 4; ++id) {
        ASSERT_TRUE(log.put(id, {.sent = id, .intended = id, .tag = 0}));
    }
    EXPECT_FALSE(log.put(5, {})) << "5 would have wiped out the send time of 1";
    EXPECT_EQ(log.pending(), 4U);

    SendTimes times;
    ASSERT_TRUE(log.take(1, times));
    EXPECT_TRUE(log.put(5, {.sent = 50, .intended = 50, .tag = 0}));
    ASSERT_TRUE(log.take(5, times));
    EXPECT_EQ(times.sent, 50U);
}

TEST(BenchFifo, OldestComesOutFirstAcrossManyWraps) {
    Fifo<4> fifo;
    std::uint64_t out = 0;
    EXPECT_FALSE(fifo.pop(out));
    std::uint64_t next_in = 1;
    std::uint64_t next_out = 1;
    for (int round = 0; round < 50; ++round) {
        while (fifo.push(next_in)) {
            ++next_in;
        }
        ASSERT_EQ(fifo.size(), 4U);
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(fifo.pop(out));
            ASSERT_EQ(out, next_out);
            ++next_out;
        }
        ASSERT_EQ(fifo.size(), 1U);
    }
    EXPECT_EQ(next_in - next_out, 1U) << "an id was lost or made up";
}

TEST(BenchStopwatch, MeasuresRealTimeAndNeverGoesNegative) {
    const auto watch = std::make_unique<Stopwatch>();
    ASSERT_EQ(watch->calibrate(), core::Status::Ok);
    EXPECT_GT(watch->overhead_ticks(), 0U);
    EXPECT_EQ(watch->overhead().count(), Stopwatch::kOverheadSamples);
    EXPECT_EQ(watch->overhead_ticks(), watch->overhead().min())
        << "the amount taken off must be the smallest cost seen, never more";

    EXPECT_EQ(watch->adjust(0), 0U);
    EXPECT_EQ(watch->adjust(watch->overhead_ticks()), 0U);
    EXPECT_EQ(watch->adjust(watch->overhead_ticks() + 5), 5U);

    // Compare with CLOCK_MONOTONIC_RAW, the clock the stopwatch is calibrated
    // against, and not with the length of the sleep. The sleep runs on
    // CLOCK_MONOTONIC, which some hosts (WSL2 among them) run at a visibly
    // different rate. The stopwatch brackets the two kernel readings, so it
    // can only have seen more time than they did.
    const std::uint64_t started = Stopwatch::start();
    const std::uint64_t raw_before = sys::monotonic_now().raw();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t raw_after = sys::monotonic_now().raw();
    const double nanos = watch->to_nanos(watch->stop(started));
    const double raw_nanos = static_cast<double>(raw_after - raw_before);
    EXPECT_GT(raw_nanos, 1e6) << "the sleep did not sleep";
    EXPECT_GE(nanos, raw_nanos * 0.99) << "the stopwatch saw less time than the kernel did";
    EXPECT_LT(nanos, 2000e6);

    const std::uint64_t ticks = watch->from_nanos(1'000'000);
    EXPECT_NEAR(watch->to_nanos(ticks), 1e6, 10.0);
}

}  // namespace
}  // namespace planck::bench
