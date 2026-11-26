#include "planck/perf/histogram.hpp"

#include <algorithm>
#include <cmath>

namespace planck::perf {

std::uint64_t Histogram::percentile(double fraction) const noexcept {
    PLANCK_EXPECT(fraction >= 0.0 && fraction <= 1.0);
    if (empty()) {
        return 0;
    }
    // How many samples must be at or below the answer. At least one, so that
    // fraction 0 gives the smallest sample's bucket.
    const double scaled = std::ceil(fraction * static_cast<double>(count_));
    const std::uint64_t needed =
        std::clamp<std::uint64_t>(static_cast<std::uint64_t>(scaled), 1, count_);

    std::uint64_t seen = 0;
    for (std::size_t index = 0; index < kBuckets; ++index) {
        seen += counts_[index];
        if (seen >= needed) {
            // The top of the bucket is a safe upper bound. The true largest
            // sample is known exactly, so never report past it.
            return std::min(highest_in(index), max_);
        }
    }
    return max_;
}

Summary Histogram::summary() const noexcept {
    return Summary{.count = count_,
                   .min = min(),
                   .p50 = percentile(0.50),
                   .p90 = percentile(0.90),
                   .p99 = percentile(0.99),
                   .p999 = percentile(0.999),
                   .p9999 = percentile(0.9999),
                   .max = max_,
                   .mean = mean()};
}

void Histogram::merge(const Histogram& other) noexcept {
    for (std::size_t index = 0; index < kBuckets; ++index) {
        counts_[index] += other.counts_[index];
    }
    count_ += other.count_;
    sum_ += other.sum_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
}

void Histogram::reset() noexcept {
    counts_.fill(0);
    count_ = 0;
    sum_ = 0;
    min_ = ~std::uint64_t{0};
    max_ = 0;
}

}  // namespace planck::perf
