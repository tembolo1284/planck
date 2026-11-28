#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "planck/perf/histogram.hpp"

namespace planck::perf {
namespace {

TEST(HistogramBuckets, SmallValuesAreExact) {
    for (std::uint64_t value = 0; value < 64; ++value) {
        const std::size_t index = Histogram::index_of(value);
        EXPECT_EQ(index, value);
        EXPECT_EQ(Histogram::lowest_in(index), value);
        EXPECT_EQ(Histogram::highest_in(index), value);
    }
}

TEST(HistogramBuckets, EveryValueFallsInsideItsBucket) {
    // Around every power of two, and at the extremes.
    std::vector<std::uint64_t> probes{0, 1, 63, 64, 65, ~std::uint64_t{0}, (~std::uint64_t{0}) - 1};
    for (unsigned bit = 6; bit < 64; ++bit) {
        const std::uint64_t power = std::uint64_t{1} << bit;
        for (const std::uint64_t value : {power - 1, power, power + 1, power + (power / 3)}) {
            probes.push_back(value);
        }
    }
    for (const std::uint64_t value : probes) {
        const std::size_t index = Histogram::index_of(value);
        ASSERT_LT(index, Histogram::kBuckets) << value;
        EXPECT_LE(Histogram::lowest_in(index), value) << value;
        EXPECT_GE(Histogram::highest_in(index), value) << value;
    }
}

TEST(HistogramBuckets, AreContiguousAndNeverWiderThanThreePercent) {
    for (std::size_t index = 0; index + 1 < Histogram::kBuckets; ++index) {
        ASSERT_EQ(Histogram::highest_in(index) + 1, Histogram::lowest_in(index + 1))
            << "gap or overlap after bucket " << index;
        const std::uint64_t low = Histogram::lowest_in(index);
        const std::uint64_t width = Histogram::highest_in(index) - low + 1;
        ASSERT_LE(width * 32, std::max<std::uint64_t>(low, 32)) << "bucket " << index << " too wide";
    }
    EXPECT_EQ(Histogram::highest_in(Histogram::kBuckets - 1), ~std::uint64_t{0});
}

TEST(HistogramTest, EmptyReportsZeros) {
    const auto histogram = std::make_unique<Histogram>();
    EXPECT_TRUE(histogram->empty());
    EXPECT_EQ(histogram->min(), 0U);
    EXPECT_EQ(histogram->max(), 0U);
    EXPECT_EQ(histogram->percentile(0.99), 0U);
    EXPECT_EQ(histogram->mean(), 0.0);
    EXPECT_EQ(histogram->summary().count, 0U);
}

TEST(HistogramTest, TracksCountMinMaxAndMeanExactly) {
    const auto histogram = std::make_unique<Histogram>();
    for (const std::uint64_t value : {500U, 100U, 900U, 100U}) {
        histogram->record(value);
    }
    EXPECT_EQ(histogram->count(), 4U);
    EXPECT_EQ(histogram->min(), 100U);
    EXPECT_EQ(histogram->max(), 900U);
    EXPECT_DOUBLE_EQ(histogram->mean(), 400.0);
    EXPECT_EQ(histogram->percentile(1.0), 900U) << "the top percentile is the largest sample, exactly";
}

TEST(HistogramTest, AValueSeenManyTimesAtOnceCountsThatManyTimes) {
    Histogram at_once;
    Histogram one_by_one;
    at_once.record(7, 3);
    at_once.record(1'000'000, 2);
    at_once.record(1, 0);  // seen no times: not seen, whatever the value
    at_once.record(2'000'000, 0);
    for (int i = 0; i < 3; ++i) {
        one_by_one.record(7);
    }
    one_by_one.record(1'000'000);
    one_by_one.record(1'000'000);
    EXPECT_EQ(at_once.count(), 5U);
    EXPECT_EQ(at_once.min(), 7U);
    EXPECT_EQ(at_once.max(), 1'000'000U);
    EXPECT_DOUBLE_EQ(at_once.mean(), one_by_one.mean());
    for (const double fraction : {0.0, 0.5, 0.6, 0.61, 0.99, 1.0}) {
        EXPECT_EQ(at_once.percentile(fraction), one_by_one.percentile(fraction)) << fraction;
    }
    Histogram nothing;
    nothing.record(55, 0);
    EXPECT_TRUE(nothing.empty());
    EXPECT_EQ(nothing.min(), 0U);
}

TEST(HistogramTest, PercentilesOfSmallValuesAreExact) {
    const auto histogram = std::make_unique<Histogram>();
    for (std::uint64_t value = 1; value <= 50; ++value) {
        histogram->record(value);
    }
    EXPECT_EQ(histogram->percentile(0.0), 1U);
    EXPECT_EQ(histogram->percentile(0.02), 1U);
    EXPECT_EQ(histogram->percentile(0.5), 25U);
    EXPECT_EQ(histogram->percentile(0.9), 45U);
    EXPECT_EQ(histogram->percentile(1.0), 50U);
}

TEST(HistogramTest, PercentilesStayWithinThreePercentOfTheTruth) {
    // A long-tailed sample, like latency: mostly small, a few enormous.
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> shape(7.0, 1.5);
    std::vector<std::uint64_t> samples;
    const auto histogram = std::make_unique<Histogram>();
    for (int i = 0; i < 200'000; ++i) {
        const auto value = static_cast<std::uint64_t>(shape(rng));
        samples.push_back(value);
        histogram->record(value);
    }
    std::sort(samples.begin(), samples.end());

    for (const double fraction : {0.5, 0.9, 0.99, 0.999, 0.9999}) {
        const auto rank = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(samples.size())));
        const std::uint64_t exact = samples[rank - 1];
        const std::uint64_t reported = histogram->percentile(fraction);
        EXPECT_GE(reported, exact) << "a percentile must never understate, at " << fraction;
        EXPECT_LE(static_cast<double>(reported), static_cast<double>(exact) * (1.0 + 1.0 / 32.0) + 1.0)
            << "more than 1/32 above the truth at " << fraction;
    }
    const Summary summary = histogram->summary();
    EXPECT_EQ(summary.count, samples.size());
    EXPECT_EQ(summary.min, samples.front());
    EXPECT_EQ(summary.max, samples.back());
    EXPECT_LE(summary.p50, summary.p90);
    EXPECT_LE(summary.p90, summary.p99);
    EXPECT_LE(summary.p99, summary.p999);
    EXPECT_LE(summary.p999, summary.p9999);
    EXPECT_LE(summary.p9999, summary.max);
}

TEST(HistogramTest, HoldsTheWholeRangeOfSixtyFourBits) {
    const auto histogram = std::make_unique<Histogram>();
    histogram->record(0);
    histogram->record(~std::uint64_t{0});
    EXPECT_EQ(histogram->min(), 0U);
    EXPECT_EQ(histogram->max(), ~std::uint64_t{0});
    EXPECT_EQ(histogram->percentile(0.5), 0U);
    EXPECT_EQ(histogram->percentile(1.0), ~std::uint64_t{0});
}

TEST(HistogramTest, MergeIsTheSameAsRecordingEverythingInOne) {
    const auto first = std::make_unique<Histogram>();
    const auto second = std::make_unique<Histogram>();
    const auto both = std::make_unique<Histogram>();
    std::mt19937_64 rng(3);
    for (int i = 0; i < 5000; ++i) {
        const std::uint64_t value = rng() % 1'000'000;
        ((i % 3 == 0) ? first : second)->record(value);
        both->record(value);
    }
    // Put the overall smallest and largest samples in the histogram being
    // merged in, so the result has to take both from there.
    for (const std::uint64_t extreme : {std::uint64_t{0}, std::uint64_t{5'000'000}}) {
        second->record(extreme);
        both->record(extreme);
    }
    first->merge(*second);
    const Summary merged = first->summary();
    const Summary direct = both->summary();
    EXPECT_EQ(merged.count, direct.count);
    EXPECT_EQ(merged.min, 0U);
    EXPECT_EQ(merged.max, 5'000'000U);
    EXPECT_EQ(merged.p50, direct.p50);
    EXPECT_EQ(merged.p999, direct.p999);
    EXPECT_DOUBLE_EQ(merged.mean, direct.mean);

    // Merging an empty histogram changes nothing, minimum included.
    const auto nothing = std::make_unique<Histogram>();
    first->merge(*nothing);
    EXPECT_EQ(first->summary().min, 0U);
    nothing->merge(*second);
    EXPECT_EQ(nothing->summary().min, 0U) << "merging into an empty histogram takes the other's minimum";
}

TEST(HistogramTest, ResetForgetsEverything) {
    const auto histogram = std::make_unique<Histogram>();
    histogram->record(123456);
    histogram->reset();
    EXPECT_TRUE(histogram->empty());
    EXPECT_EQ(histogram->max(), 0U);
    histogram->record(7);
    EXPECT_EQ(histogram->min(), 7U);
    EXPECT_EQ(histogram->percentile(1.0), 7U);
}

}  // namespace
}  // namespace planck::perf
