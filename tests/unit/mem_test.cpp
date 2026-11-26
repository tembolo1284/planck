#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "planck/core/types.hpp"
#include "planck/mem/aligned.hpp"
#include "planck/mem/arena.hpp"
#include "planck/mem/pool.hpp"

namespace planck::mem {
namespace {

using core::Status;

struct Item {
    Index next;
    std::uint32_t value;
    std::uint64_t tag;
};
static_assert(Poolable<Item> && ArenaStorable<Item>);
static_assert(!Poolable<std::uint16_t>, "too small to hold a free-list link");

std::uintptr_t address_of(const std::byte* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

TEST(Arena, UninitialisedHandsOutNothing) {
    Arena arena;
    EXPECT_FALSE(arena.initialised());
    EXPECT_EQ(arena.allocate(8, 8), nullptr);
    EXPECT_TRUE(arena.allocate_array<int>(4).empty());
    EXPECT_EQ(arena.init(0, sys::HugePages::Off), Status::InvalidArgument);
    EXPECT_FALSE(arena.initialised());
}

TEST(Arena, BumpsAndAligns) {
    Arena arena;
    ASSERT_EQ(arena.init(1 << 20, sys::HugePages::Off), Status::Ok);
    EXPECT_EQ(arena.init(4096, sys::HugePages::Off), Status::Duplicate);
    EXPECT_EQ(arena.capacity(), 1U << 20);

    const std::byte* const first = arena.allocate(3, 1);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(arena.used(), 3U);

    const std::byte* const second = arena.allocate(64, 64);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(address_of(second) % 64, 0U);
    EXPECT_EQ(second - first, 64);
    EXPECT_EQ(arena.used(), 128U);

    const std::byte* const third = arena.allocate(1, 4096);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(address_of(third) % 4096, 0U);

    EXPECT_EQ(arena.allocate(0, 8), nullptr);
}

TEST(Arena, ExhaustionIsCleanAndLeavesStateAlone) {
    Arena arena;
    ASSERT_EQ(arena.init(8192, sys::HugePages::Off), Status::Ok);
    ASSERT_NE(arena.allocate(100, 1), nullptr);
    const std::size_t used = arena.used();

    EXPECT_EQ(arena.allocate(8192, 1), nullptr);
    EXPECT_TRUE(arena.allocate_array<std::uint64_t>(SIZE_MAX / 4).empty()) << "size overflow";
    EXPECT_EQ(arena.used(), used);

    EXPECT_NE(arena.allocate(arena.remaining(), 1), nullptr);
    EXPECT_EQ(arena.remaining(), 0U);
    EXPECT_EQ(arena.allocate(1, 1), nullptr);
}

TEST(Arena, ArraysStartZeroedAndSurvivePrefault) {
    Arena arena;
    ASSERT_EQ(arena.init(1 << 20, sys::HugePages::Off), Status::Ok);
    const auto values = arena.allocate_array<core::Qty>(1000);
    ASSERT_EQ(values.size(), 1000U);
    EXPECT_TRUE(std::ranges::all_of(values, [](core::Qty qty) { return qty == core::Qty{0}; }));

    values[7] = core::Qty{42};
    arena.prefault();
    EXPECT_EQ(values[7], core::Qty{42});
}

TEST(Arena, MoveTransfersOwnership) {
    Arena source;
    ASSERT_EQ(source.init(1 << 16, sys::HugePages::Off), Status::Ok);
    ASSERT_NE(source.allocate(10, 1), nullptr);

    Arena moved{std::move(source)};
    EXPECT_FALSE(source.initialised());  // NOLINT(bugprone-use-after-move): that is the point
    EXPECT_EQ(source.capacity(), 0U);
    EXPECT_TRUE(moved.initialised());
    EXPECT_EQ(moved.used(), 10U);

    Arena target;
    ASSERT_EQ(target.init(4096, sys::HugePages::Off), Status::Ok);
    target = std::move(moved);
    EXPECT_EQ(target.capacity(), 1U << 16);
    EXPECT_FALSE(moved.initialised());  // NOLINT(bugprone-use-after-move)
}

class PoolTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(arena_.init(1 << 20, sys::HugePages::Off), Status::Ok); }

    Arena arena_;
    IndexPool<Item> pool_;
};

TEST_F(PoolTest, InitValidation) {
    EXPECT_EQ(pool_.init({}), Status::InvalidArgument);
    ASSERT_EQ(pool_.init(arena_.allocate_array<Item>(4)), Status::Ok);
    EXPECT_EQ(pool_.init(arena_.allocate_array<Item>(4)), Status::Duplicate);
    EXPECT_EQ(pool_.capacity(), 4U);
    EXPECT_EQ(pool_.in_use(), 0U);
    EXPECT_EQ(pool_.available(), 4U);
}

TEST_F(PoolTest, FreshPoolHandsOutAscendingSlots) {
    ASSERT_EQ(pool_.init(arena_.allocate_array<Item>(4)), Status::Ok);
    EXPECT_EQ(pool_.acquire(), 0U);
    EXPECT_EQ(pool_.acquire(), 1U);
    EXPECT_EQ(pool_.acquire(), 2U);
    EXPECT_EQ(pool_.in_use(), 3U);
}

TEST_F(PoolTest, ReuseIsLastInFirstOut) {
    ASSERT_EQ(pool_.init(arena_.allocate_array<Item>(8)), Status::Ok);
    const Index a = pool_.acquire();
    const Index b = pool_.acquire();
    const Index c = pool_.acquire();
    pool_.release(b);
    pool_.release(a);
    EXPECT_EQ(pool_.acquire(), a) << "most recently released comes back first";
    EXPECT_EQ(pool_.acquire(), b);
    EXPECT_EQ(pool_.acquire(), c + 1) << "then untouched slots";
}

TEST_F(PoolTest, ExhaustsThenRecovers) {
    ASSERT_EQ(pool_.init(arena_.allocate_array<Item>(3)), Status::Ok);
    const Index a = pool_.acquire();
    static_cast<void>(pool_.acquire());
    static_cast<void>(pool_.acquire());
    EXPECT_TRUE(pool_.exhausted());
    EXPECT_EQ(pool_.acquire(), kNullIndex);
    EXPECT_EQ(pool_.in_use(), 3U);

    pool_.release(a);
    EXPECT_FALSE(pool_.exhausted());
    EXPECT_EQ(pool_.acquire(), a);
}

TEST_F(PoolTest, EverySlotIsDistinctAndWritable) {
    constexpr Index kCount = 10'000;
    ASSERT_EQ(pool_.init(arena_.allocate_array<Item>(kCount)), Status::Ok);

    std::vector<Index> taken;
    for (Index slot = pool_.acquire(); slot != kNullIndex; slot = pool_.acquire()) {
        pool_[slot] = Item{.next = 0, .value = slot, .tag = std::uint64_t{slot} * 3};
        taken.push_back(slot);
    }
    ASSERT_EQ(taken.size(), kCount);

    std::ranges::sort(taken);
    EXPECT_EQ(std::ranges::adjacent_find(taken), taken.end()) << "a slot was handed out twice";
    for (const Index slot : taken) {
        ASSERT_EQ(pool_[slot].value, slot);
        ASSERT_EQ(pool_[slot].tag, std::uint64_t{slot} * 3);
        ASSERT_EQ(pool_.index_of(pool_[slot]), slot);
    }
}

TEST_F(PoolTest, FreeSlotKeepsBytesPastItsLink) {
    // OrderStore relies on this to tell free slots from live ones.
    ASSERT_EQ(pool_.init(arena_.allocate_array<Item>(2)), Status::Ok);
    const Index slot = pool_.acquire();
    pool_[slot] = Item{.next = 0, .value = 77, .tag = 0xABCDEF};
    pool_.release(slot);
    EXPECT_EQ(pool_[slot].value, 77U);
    EXPECT_EQ(pool_[slot].tag, 0xABCDEFU);
}

TEST(CacheAligned, OccupiesWholeLines) {
    EXPECT_EQ(alignof(CacheAligned<char>), core::kCacheLine);
    EXPECT_EQ(sizeof(CacheAligned<char>), core::kCacheLine);
    struct Pair {
        CacheAligned<int> a;
        CacheAligned<int> b;
    };
    EXPECT_EQ(offsetof(Pair, b) - offsetof(Pair, a), core::kCacheLine);
}

}  // namespace
}  // namespace planck::mem
