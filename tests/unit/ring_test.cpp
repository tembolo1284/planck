#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "planck/mem/arena.hpp"
#include "planck/ring/spsc.hpp"

namespace planck::ring {
namespace {

using core::Status;

// Single-threaded behaviour. The two-thread test is in tests/integration.
class RingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(arena_.init(1 << 16, sys::HugePages::Off), Status::Ok);
        ASSERT_EQ(ring_.init(arena_.allocate_array<int>(4)), Status::Ok);
    }

    std::vector<int> drain_all() {
        std::vector<int> out;
        ring_.drain(1000, [&out](const int& value) noexcept { out.push_back(value); });
        return out;
    }

    mem::Arena arena_;
    SpscRing<int> ring_;
};

TEST_F(RingTest, InitValidation) {
    SpscRing<int> ring;
    EXPECT_EQ(ring.init({}), Status::InvalidArgument);
    EXPECT_EQ(ring.init(arena_.allocate_array<int>(1)), Status::InvalidArgument);
    EXPECT_EQ(ring.init(arena_.allocate_array<int>(3)), Status::InvalidArgument) << "not a power of two";
    EXPECT_EQ(ring.init(arena_.allocate_array<int>(8)), Status::Ok);
    EXPECT_EQ(ring.init(arena_.allocate_array<int>(8)), Status::Duplicate);
    EXPECT_EQ(ring.capacity(), 8U);
}

TEST_F(RingTest, EmptyRingGivesNothing) {
    int value = -1;
    EXPECT_FALSE(ring_.try_pop(value));
    EXPECT_EQ(value, -1) << "a failed pop must not touch the output";
    EXPECT_TRUE(drain_all().empty());
    EXPECT_EQ(ring_.size_approx(), 0U);
}

TEST_F(RingTest, FifoOrder) {
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring_.try_push(i));
    }
    EXPECT_EQ(drain_all(), (std::vector<int>{0, 1, 2, 3}));
}

TEST_F(RingTest, UsesEverySlotThenRefuses) {
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring_.try_push(i));
    }
    EXPECT_FALSE(ring_.try_push(99));
    EXPECT_EQ(ring_.size_approx(), 4U);

    int value = 0;
    ASSERT_TRUE(ring_.try_pop(value));
    EXPECT_EQ(value, 0);
    EXPECT_TRUE(ring_.try_push(4));
    EXPECT_FALSE(ring_.try_push(5));
}

TEST_F(RingTest, DrainRespectsItsLimit) {
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring_.try_push(i));
    }
    int sum = 0;
    EXPECT_EQ(ring_.drain(3, [&sum](const int& value) noexcept { sum += value; }), 3U);
    EXPECT_EQ(sum, 0 + 1 + 2);
    EXPECT_EQ(ring_.size_approx(), 1U);
    EXPECT_EQ(ring_.drain(0, [](const int&) noexcept {}), 0U);
}

TEST_F(RingTest, DrainSeesEverythingQueued) {
    // The consumer keeps a private copy of the producer's index. drain() must
    // refresh it, or it would return fewer elements than are waiting.
    ASSERT_TRUE(ring_.try_push(1));
    int value = 0;
    ASSERT_TRUE(ring_.try_pop(value));
    ASSERT_TRUE(ring_.try_push(2));
    ASSERT_TRUE(ring_.try_push(3));
    ASSERT_TRUE(ring_.try_push(4));
    EXPECT_EQ(drain_all(), (std::vector<int>{2, 3, 4}));
}

TEST_F(RingTest, PushAllIsAllOrNothing) {
    ASSERT_TRUE(ring_.try_push(1));
    ASSERT_TRUE(ring_.try_push(2));

    const std::array<int, 3> too_many{10, 11, 12};
    EXPECT_FALSE(ring_.try_push_all(too_many));
    EXPECT_EQ(ring_.size_approx(), 2U) << "a refused batch must leave nothing behind";

    const std::array<int, 2> fits{10, 11};
    EXPECT_TRUE(ring_.try_push_all(fits));
    EXPECT_TRUE(ring_.try_push_all(std::span<const int>{}));
    EXPECT_FALSE(ring_.try_push_all(std::span<const int>{fits}.first(1)));
    EXPECT_EQ(drain_all(), (std::vector<int>{1, 2, 10, 11}));
}

TEST_F(RingTest, WrapsManyTimes) {
    SpscRing<std::uint64_t> tiny;
    ASSERT_EQ(tiny.init(arena_.allocate_array<std::uint64_t>(2)), Status::Ok);

    std::uint64_t next_in = 0;
    std::uint64_t next_out = 0;
    for (int lap = 0; lap < 50'000; ++lap) {
        while (tiny.try_push(next_in)) {
            ++next_in;
        }
        std::uint64_t value = 0;
        while (tiny.try_pop(value)) {
            ASSERT_EQ(value, next_out);
            ++next_out;
        }
    }
    EXPECT_EQ(next_in, 100'000U);
    EXPECT_EQ(next_out, next_in);
}

TEST(RingLayout, ProducerAndConsumerDoNotShareALine) {
    EXPECT_EQ(alignof(SpscRing<int>), core::kCacheLine);
    EXPECT_EQ(sizeof(SpscRing<int>), 2 * core::kCacheLine);
}

}  // namespace
}  // namespace planck::ring
