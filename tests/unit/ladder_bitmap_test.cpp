#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "planck/book/ladder_bitmap.hpp"
#include "planck/mem/arena.hpp"

namespace planck::book {
namespace {

using core::Status;

class LadderBitmapTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(arena_.init(1 << 20, sys::HugePages::Off), Status::Ok); }

    void init(LadderBitmap& bitmap, std::uint32_t bits) {
        ASSERT_EQ(bitmap.init(arena_.allocate_array<std::uint64_t>(bits / 64), bits), Status::Ok);
    }

    mem::Arena arena_;
};

TEST_F(LadderBitmapTest, InitValidation) {
    LadderBitmap bitmap;
    EXPECT_EQ(bitmap.init(arena_.allocate_array<std::uint64_t>(2), 100), Status::InvalidArgument);
    EXPECT_EQ(bitmap.init(arena_.allocate_array<std::uint64_t>(3), 128), Status::InvalidArgument);
    EXPECT_EQ(bitmap.init({}, 0), Status::InvalidArgument);
    EXPECT_EQ(bitmap.init(arena_.allocate_array<std::uint64_t>(2), 128), Status::Ok);
    EXPECT_TRUE(bitmap.empty());
    EXPECT_EQ(bitmap.size(), 128U);
}

TEST_F(LadderBitmapTest, EmptyFindsNothing) {
    LadderBitmap bitmap;
    init(bitmap, 4096);
    EXPECT_EQ(bitmap.lowest_from(0), LadderBitmap::kNone);
    EXPECT_EQ(bitmap.highest_upto(4095), LadderBitmap::kNone);
}

TEST_F(LadderBitmapTest, SearchIsInclusiveOfTheStartBit) {
    LadderBitmap bitmap;
    init(bitmap, 4096);
    bitmap.set(100);
    EXPECT_TRUE(bitmap.test(100));
    EXPECT_FALSE(bitmap.test(101));
    EXPECT_EQ(bitmap.lowest_from(100), 100U);
    EXPECT_EQ(bitmap.highest_upto(100), 100U);
    EXPECT_EQ(bitmap.lowest_from(101), LadderBitmap::kNone);
    EXPECT_EQ(bitmap.highest_upto(99), LadderBitmap::kNone);
}

TEST_F(LadderBitmapTest, FindsAcrossWordsAndGroups) {
    LadderBitmap bitmap;
    init(bitmap, LadderBitmap::kMaxBits);
    const std::uint32_t last = LadderBitmap::kMaxBits - 1;

    // Far apart, so the search has to climb to the top tier and back down.
    bitmap.set(5);
    bitmap.set(last);
    EXPECT_EQ(bitmap.lowest_from(6), last);
    EXPECT_EQ(bitmap.highest_upto(last - 1), 5U);

    // 63 -> 64 crosses a word. 4095 -> 4096 crosses a group of 64 words.
    bitmap.set(64);
    bitmap.set(4096);
    EXPECT_EQ(bitmap.lowest_from(6), 64U);
    EXPECT_EQ(bitmap.lowest_from(65), 4096U);
    EXPECT_EQ(bitmap.highest_upto(4095), 64U);
    EXPECT_EQ(bitmap.highest_upto(63), 5U);
}

TEST_F(LadderBitmapTest, ClearingTheLastBitEmptiesEveryTier) {
    LadderBitmap bitmap;
    init(bitmap, LadderBitmap::kMaxBits);
    bitmap.set(70'000);
    bitmap.set(70'001);
    bitmap.clear(70'000);
    EXPECT_FALSE(bitmap.empty());
    EXPECT_EQ(bitmap.lowest_from(0), 70'001U);
    bitmap.clear(70'001);
    EXPECT_TRUE(bitmap.empty());
    EXPECT_EQ(bitmap.lowest_from(0), LadderBitmap::kNone);
    EXPECT_EQ(bitmap.highest_upto(LadderBitmap::kMaxBits - 1), LadderBitmap::kNone);
}

TEST_F(LadderBitmapTest, AgreesWithALinearScan) {
    for (const std::uint32_t bits : {64U, 128U, 4096U, 8192U, LadderBitmap::kMaxBits}) {
        SCOPED_TRACE(bits);
        LadderBitmap bitmap;
        init(bitmap, bits);
        std::vector<bool> model(bits, false);
        std::mt19937_64 rng(bits);

        for (int step = 0; step < 20'000; ++step) {
            // Mostly a few sparse clusters, so the upper tiers get exercised.
            const bool anywhere = (rng() % 4 == 0);
            const auto cluster = static_cast<std::uint32_t>((rng() % 8) * (bits / 8) + (rng() % 3));
            const std::uint32_t bit = anywhere ? static_cast<std::uint32_t>(rng() % bits) : cluster % bits;
            if (rng() % 3 != 0) {
                bitmap.set(bit);
                model[bit] = true;
            } else {
                bitmap.clear(bit);
                model[bit] = false;
            }

            const auto query = static_cast<std::uint32_t>(rng() % bits);
            std::uint32_t lowest = LadderBitmap::kNone;
            for (std::uint32_t i = query; i < bits; ++i) {
                if (model[i]) {
                    lowest = i;
                    break;
                }
            }
            std::uint32_t highest = LadderBitmap::kNone;
            for (std::uint32_t i = query + 1; i-- > 0;) {
                if (model[i]) {
                    highest = i;
                    break;
                }
            }
            ASSERT_EQ(bitmap.lowest_from(query), lowest) << "step " << step << " query " << query;
            ASSERT_EQ(bitmap.highest_upto(query), highest) << "step " << step << " query " << query;
            ASSERT_EQ(bitmap.test(query), model[query]);
        }
    }
}

}  // namespace
}  // namespace planck::book
