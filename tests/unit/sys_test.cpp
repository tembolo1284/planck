#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "planck/sys/clock.hpp"
#include "planck/sys/cpu.hpp"
#include "planck/sys/error.hpp"
#include "planck/sys/memory.hpp"

namespace planck::sys {
namespace {

using core::ok;
using core::Status;

TEST(CpuList, ParsesKernelFormat) {
    CpuSet set;
    ASSERT_EQ(parse_cpu_list("2-5,8,10-11\n", set), Status::Ok);
    EXPECT_EQ(set.count(), 7U);
    for (const std::size_t cpu : {2U, 3U, 4U, 5U, 8U, 10U, 11U}) {
        EXPECT_TRUE(set.test(cpu)) << cpu;
    }
    for (const std::size_t cpu : {0U, 1U, 6U, 7U, 9U, 12U}) {
        EXPECT_FALSE(set.test(cpu)) << cpu;
    }
}

TEST(CpuList, EmptyMeansNoCpus) {
    CpuSet set;
    set.set(3);
    ASSERT_EQ(parse_cpu_list("", set), Status::Ok);
    EXPECT_TRUE(set.empty());
    ASSERT_EQ(parse_cpu_list("\n", set), Status::Ok);
    EXPECT_TRUE(set.empty());
}

TEST(CpuList, Limits) {
    CpuSet set;
    ASSERT_EQ(parse_cpu_list("1023", set), Status::Ok);
    EXPECT_TRUE(set.test(1023));
    EXPECT_FALSE(set.test(1024));
    EXPECT_FALSE(set.test(1'000'000));
    EXPECT_EQ(parse_cpu_list("1024", set), Status::OutOfRange);
    EXPECT_EQ(parse_cpu_list("99999999999999999999999", set), Status::OutOfRange);
}

TEST(CpuList, RejectsMalformedText) {
    for (const char* const text : {"5-2", "1,", ",1", "1,,2", "1-", "-1", "a", "1 2", "1-2-3x"}) {
        CpuSet set;
        set.set(7);
        EXPECT_EQ(parse_cpu_list(text, set), Status::Protocol) << text;
        // A failed parse leaves the output alone.
        EXPECT_TRUE(set.test(7)) << text;
        EXPECT_EQ(set.count(), 1U) << text;
    }
}

TEST(Errno, MapsToStatus) {
    EXPECT_EQ(status_from_errno(EPERM), Status::Permission);
    EXPECT_EQ(status_from_errno(EACCES), Status::Permission);
    EXPECT_EQ(status_from_errno(EAGAIN), Status::WouldBlock);
    EXPECT_EQ(status_from_errno(ENOMEM), Status::Capacity);
    EXPECT_EQ(status_from_errno(ECONNRESET), Status::Closed);
    EXPECT_EQ(status_from_errno(ENOENT), Status::NotFound);
    EXPECT_EQ(status_from_errno(123456), Status::Io);
    EXPECT_NE(status_from_errno(0), Status::Ok);
}

// Affinity and names are per thread, so these run on a throwaway thread and
// leave the test runner's own thread alone.
TEST(Thread, PinsToARequestedCpu) {
    std::thread worker([] {
        const std::size_t cpu = current_cpu();
        const Status status = pin_current_thread(cpu);
        if (!ok(status)) {
            GTEST_SKIP() << "cannot set affinity here: " << core::to_string(status);
        }
        EXPECT_EQ(current_cpu(), cpu);
        EXPECT_EQ(pin_current_thread(kMaxCpus), Status::OutOfRange);
    });
    worker.join();
}

TEST(Thread, NameLengthIsChecked) {
    std::thread worker([] {
        EXPECT_EQ(name_current_thread("planck-test"), Status::Ok);
        EXPECT_EQ(name_current_thread("exactly15chars."), Status::Ok);
        EXPECT_EQ(name_current_thread("sixteen-chars-xx"), Status::InvalidArgument);
        EXPECT_EQ(name_current_thread(""), Status::InvalidArgument);
    });
    worker.join();
}

TEST(Thread, RealtimePriorityRangeIsChecked) {
    EXPECT_EQ(set_realtime_priority(0), Status::InvalidArgument);
    EXPECT_EQ(set_realtime_priority(100), Status::InvalidArgument);
    EXPECT_EQ(set_realtime_priority(-1), Status::InvalidArgument);
}

TEST(Thread, ConfigureReportsWhatItAchieved) {
    std::thread worker([] {
        const std::size_t cpu = current_cpu();
        HotThreadReport report;
        const Status status = configure_hot_thread({.cpu = cpu, .name = "planck-hot"}, report);
        if (!ok(status)) {
            GTEST_SKIP() << "cannot set affinity here: " << core::to_string(status);
        }
        EXPECT_TRUE(report.pinned);
        EXPECT_FALSE(report.realtime);

        // Asking for an isolated core on a machine without one must fail
        // loudly, not silently run on a shared core.
        if (!report.isolated) {
            HotThreadReport strict;
            EXPECT_EQ(configure_hot_thread({.cpu = cpu, .require_isolated = true}, strict),
                      Status::Unsupported);
            EXPECT_TRUE(strict.pinned);
        }
    });
    worker.join();
}

TEST(Memory, MapsRoundsUpAndUnmaps) {
    Region region;
    EXPECT_EQ(map_region(0, HugePages::Off, region), Status::InvalidArgument);
    EXPECT_EQ(map_region(kMaxRegionBytes + 1, HugePages::Off, region), Status::InvalidArgument);

    ASSERT_EQ(map_region(100'000, HugePages::Off, region), Status::Ok);
    EXPECT_EQ(region.size, 102'400U);
    EXPECT_FALSE(region.huge);
    EXPECT_EQ(region.data[0], std::byte{0});
    EXPECT_EQ(region.data[region.size - 1], std::byte{0});

    EXPECT_EQ(unmap_region(region), Status::Ok);
    EXPECT_EQ(region.data, nullptr);
    EXPECT_EQ(unmap_region(region), Status::InvalidArgument);
}

TEST(Memory, PrefaultKeepsContents) {
    Region region;
    ASSERT_EQ(map_region(64 * 1024, HugePages::Off, region), Status::Ok);
    region.data[4096] = std::byte{0x5A};
    region.data[4097] = std::byte{0xA5};
    prefault(region);
    EXPECT_EQ(region.data[4096], std::byte{0x5A});
    EXPECT_EQ(region.data[4097], std::byte{0xA5});
    EXPECT_EQ(region.data[0], std::byte{0});
    EXPECT_EQ(unmap_region(region), Status::Ok);
}

TEST(Memory, TryFallsBackWhenNoHugePages) {
    Region region;
    ASSERT_EQ(map_region(3'000'000, HugePages::Try, region), Status::Ok);
    EXPECT_GE(region.size, 3'000'000U);
    prefault(region);
    EXPECT_EQ(unmap_region(region), Status::Ok);
}

TEST(Clock, RejectsBadCalibrationWindow) {
    TscClock clock;
    EXPECT_FALSE(clock.calibrated());
    EXPECT_EQ(clock.calibrate(core::Nanos{10}), Status::InvalidArgument);
    EXPECT_EQ(clock.calibrate(core::Nanos{5'000'000'000}), Status::InvalidArgument);
    EXPECT_FALSE(clock.calibrated());
}

TEST(Clock, AgreesWithTheKernelClock) {
    TscClock clock;
    ASSERT_EQ(clock.calibrate(), Status::Ok);
    ASSERT_TRUE(clock.calibrated());
    EXPECT_GT(clock.ticks_per_second(), 1'000'000U);

    const core::Nanos mono_start = monotonic_now();
    const std::uint64_t tsc_start = tsc_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint64_t tsc_end = tsc_now_serialized();
    const core::Nanos mono_end = monotonic_now();

    const double measured = static_cast<double>(clock.to_nanos(tsc_end - tsc_start).raw());
    const double expected = static_cast<double>((mono_end - mono_start).raw());
    // Generous: this runs on shared CI machines. A healthy box is within 50 ppm.
    EXPECT_NEAR(measured / expected, 1.0, 0.01);

    const auto skew = static_cast<std::int64_t>(clock.now().raw()) -
                      static_cast<std::int64_t>(wall_now().raw());
    EXPECT_LT(std::llabs(skew), 5'000'000) << "TscClock::now() is more than 5 ms from the wall clock";
}

TEST(Clock, CounterNeverGoesBackwards) {
    std::uint64_t previous = tsc_now();
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t now = tsc_now();
        ASSERT_GE(now, previous);
        previous = now;
    }
}

}  // namespace
}  // namespace planck::sys
