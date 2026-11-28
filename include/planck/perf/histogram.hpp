#ifndef PLANCK_PERF_HISTOGRAM_HPP
#define PLANCK_PERF_HISTOGRAM_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "planck/core/contract.hpp"

namespace planck::perf {

// What a histogram holds, boiled down. Every value is in the unit that was
// recorded. Percentiles are upper bounds: at least that share of the samples
// were at or below the figure.
struct Summary {
    std::uint64_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
    std::uint64_t p9999 = 0;
    std::uint64_t max = 0;
    double mean = 0.0;
};

// Counts how often each value was seen, for working out percentiles later.
//
// An average hides exactly what a latency measurement is for: the rare slow
// case. Keeping every sample would cost memory that grows without bound. So
// values are sorted into buckets instead. Small values (below 64) each get
// their own bucket. Above that, every power of two is split into 32 equal
// buckets, which keeps the error under 1/32, about 3 percent, at any size.
// The whole thing is 1920 counters, 15 KiB, fixed.
//
// record() is a shift, an add and an increment. It never allocates, never
// branches on the data in a way that matters, and covers the full range of
// a 64-bit value. One thread writes. Reading from another thread while it
// writes is a race: copy or summarise on the writing thread.
class Histogram {
public:
    static constexpr unsigned kSubBucketBits = 5;
    static constexpr std::size_t kSubBuckets = std::size_t{1} << kSubBucketBits;
    static constexpr std::size_t kBuckets = (64 - kSubBucketBits + 1) * kSubBuckets;

    void record(std::uint64_t value) noexcept {
        ++counts_[index_of(value)];
        ++count_;
        sum_ += value;
        min_ = (value < min_) ? value : min_;
        max_ = (value > max_) ? value : max_;
    }

    // The same value seen `times` times, at the cost of seeing it once.
    void record(std::uint64_t value, std::uint64_t times) noexcept {
        if (times == 0) {
            return;
        }
        counts_[index_of(value)] += times;
        count_ += times;
        sum_ += value * times;
        min_ = (value < min_) ? value : min_;
        max_ = (value > max_) ? value : max_;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    // Zero when empty.
    [[nodiscard]] std::uint64_t min() const noexcept { return empty() ? 0 : min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return empty() ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    // The smallest figure that at least `fraction` of the samples were at or
    // below, to the histogram's precision. fraction runs from 0 to 1. The
    // result is exact for the largest sample and never exceeds it. Zero when
    // empty.
    [[nodiscard]] std::uint64_t percentile(double fraction) const noexcept;

    [[nodiscard]] Summary summary() const noexcept;

    // Add another histogram's samples to this one.
    void merge(const Histogram& other) noexcept;
    void reset() noexcept;

    // Which bucket a value lands in, and the range a bucket covers. Public so
    // tests can check the arithmetic.
    [[nodiscard]] static constexpr std::size_t index_of(std::uint64_t value) noexcept {
        if (value < (2 * kSubBuckets)) {
            return static_cast<std::size_t>(value);
        }
        // The position of the top set bit decides the power of two. The five
        // bits below it decide the bucket within that power.
        const unsigned shift =
            static_cast<unsigned>(std::bit_width(value)) - 1U - kSubBucketBits;
        return (shift * kSubBuckets) + static_cast<std::size_t>(value >> shift);
    }
    [[nodiscard]] static constexpr std::uint64_t lowest_in(std::size_t index) noexcept {
        if (index < (2 * kSubBuckets)) {
            return index;
        }
        const std::size_t shift = (index / kSubBuckets) - 1;
        return static_cast<std::uint64_t>(index - (shift * kSubBuckets)) << shift;
    }
    [[nodiscard]] static constexpr std::uint64_t highest_in(std::size_t index) noexcept {
        if (index < (2 * kSubBuckets)) {
            return index;
        }
        const std::size_t shift = (index / kSubBuckets) - 1;
        return lowest_in(index) + ((std::uint64_t{1} << shift) - 1);
    }

private:
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = ~std::uint64_t{0};
    std::uint64_t max_ = 0;
    std::array<std::uint64_t, kBuckets> counts_{};
};

static_assert(Histogram::index_of(~std::uint64_t{0}) == Histogram::kBuckets - 1);
static_assert(Histogram::index_of(63) == 63 && Histogram::index_of(64) == 64);

}  // namespace planck::perf

#endif  // PLANCK_PERF_HISTOGRAM_HPP
