#ifndef PLANCK_BENCH_HARNESS_HPP
#define PLANCK_BENCH_HARNESS_HPP

#include <cstdint>
#include <string_view>

#include "planck/core/status.hpp"
#include "planck/perf/histogram.hpp"
#include "planck/sys/clock.hpp"

// Shared by the benchmark programs: a stopwatch that knows what its own
// reading costs, and a table printer.
namespace planck::bench {

// Tell the compiler a value is used, so the work that produced it is not
// thrown away as dead code. Costs nothing at run time.
template <typename T>
[[gnu::always_inline]] inline void keep(const T& value) noexcept {
    __asm__ __volatile__("" : : "r,m"(value) : "memory");
}

// Times short pieces of work with the cycle counter.
//
//     const std::uint64_t start = watch.start();
//     work();
//     histogram.record(watch.stop(start));
//
// Reading the counter is not free: a start and a stop with nothing between
// them still measure twenty to forty cycles. For work that itself takes
// twenty cycles that would double the answer. So calibrate() measures the
// empty interval, and stop() takes the smallest one ever seen off every
// result. Smallest, not typical, so that no result is ever made to look
// better than it was.
class Stopwatch {
public:
    static constexpr std::uint64_t kOverheadSamples = 200'000;

    // Calibrate the clock and measure the cost of an empty interval. Run it
    // on the CPU that will do the timing. Blocks for about 50 ms.
    [[nodiscard]] core::Status calibrate() noexcept;

    [[nodiscard]] static std::uint64_t start() noexcept { return sys::tsc_now(); }

    // Ticks since `started`, less the cost of measuring.
    [[nodiscard]] std::uint64_t stop(std::uint64_t started) const noexcept {
        return adjust(sys::tsc_now_serialized() - started);
    }

    // Take the measuring cost off an interval that was timed some other way.
    [[nodiscard]] std::uint64_t adjust(std::uint64_t ticks) const noexcept {
        return (ticks > overhead_ticks_) ? (ticks - overhead_ticks_) : 0;
    }

    [[nodiscard]] double to_nanos(std::uint64_t ticks) const noexcept {
        return static_cast<double>(ticks) * nanos_per_tick_;
    }
    [[nodiscard]] std::uint64_t from_nanos(std::uint64_t nanos) const noexcept {
        return static_cast<std::uint64_t>(static_cast<double>(nanos) / nanos_per_tick_);
    }

    [[nodiscard]] std::uint64_t overhead_ticks() const noexcept { return overhead_ticks_; }
    [[nodiscard]] std::uint64_t ticks_per_second() const noexcept { return clock_.ticks_per_second(); }
    // Empty intervals exactly as measured, with nothing taken off.
    [[nodiscard]] const perf::Histogram& overhead() const noexcept { return overhead_; }

private:
    sys::TscClock clock_{};
    double nanos_per_tick_ = 0.0;
    std::uint64_t overhead_ticks_ = 0;
    perf::Histogram overhead_{};
};

// Prints one line per histogram, in nanoseconds.
class Table {
public:
    Table(const Stopwatch& watch, bool csv) noexcept : watch_(&watch), csv_(csv) {}

    void header() const noexcept;
    // `ticks` holds intervals in cycle counter ticks.
    void row(std::string_view name, const perf::Histogram& ticks) const noexcept;

private:
    const Stopwatch* watch_;
    bool csv_;
};

// One line saying what the numbers were measured on: CPU count, counter
// rate, whether the build is optimised and what the contracts do.
void print_environment(const Stopwatch& watch) noexcept;

}  // namespace planck::bench

#endif  // PLANCK_BENCH_HARNESS_HPP
