#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

#include "planck/core/bits.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"

namespace planck::core {
namespace {

// Most of core is constexpr, so most of its tests run in the compiler.
static_assert(is_pow2(std::uint32_t{64}) && !is_pow2(std::uint32_t{0}) && !is_pow2(std::uint32_t{6}));
static_assert(ceil_pow2(1) == 1 && ceil_pow2(5) == 8 && ceil_pow2(8) == 8);
static_assert(align_up(0, 64) == 0 && align_up(1, 64) == 64 && align_up(64, 64) == 64);
static_assert(is_aligned(128, 64) && !is_aligned(100, 64));
static_assert(lowest_set(std::uint64_t{0b101000}) == 3 && highest_set(std::uint64_t{0b101000}) == 5);
static_assert(highest_set(std::uint8_t{0x80}) == 7 && lowest_set(std::uint64_t{1} << 63) == 63);

static_assert((Price{5} + Price{-7}) == Price{-2} && (Qty{9} - Qty{4}) == Qty{5});
static_assert(Price{1} < Price{2} && Price::min() < Price{0} && Qty::max() > Qty{0});
static_assert(opposite(Side::Buy) == Side::Sell && opposite(Side::Sell) == Side::Buy);
static_assert(index_of(Side::Buy) == 0 && index_of(Side::Sell) == 1);

// Strong types must not convert to each other or from raw integers by accident.
static_assert(!std::is_convertible_v<int, Price> && !std::is_convertible_v<Price, int>);
static_assert(!std::is_convertible_v<Price, Qty> && !std::is_convertible_v<OrderId, SeqNo>);

// Only quantities that opt in have arithmetic. Adding two order ids is a bug.
template <typename T>
concept Addable = requires(T a) { a + a; };
static_assert(Addable<Price> && Addable<Qty> && Addable<Nanos>);
static_assert(!Addable<OrderId> && !Addable<SymbolId> && !Addable<SessionId>);

TEST(Strong, CompoundAssignment) {
    Qty qty{3};
    qty += Qty{4};
    qty -= Qty{2};
    EXPECT_EQ(qty, Qty{5});
    EXPECT_EQ(qty.raw(), 5U);
}

TEST(Strong, DefaultIsZero) {
    EXPECT_EQ(Price{}.raw(), 0);
    EXPECT_EQ(OrderId{}, kNoOrderId);
}

TEST(Enums, RejectOutOfRangeBytes) {
    EXPECT_TRUE(is_valid(Side::Buy));
    EXPECT_TRUE(is_valid(Side::Sell));
    EXPECT_FALSE(is_valid(static_cast<Side>(2)));
    EXPECT_FALSE(is_valid(static_cast<Side>(255)));
    EXPECT_TRUE(is_valid(OrderType::Market));
    EXPECT_FALSE(is_valid(static_cast<OrderType>(2)));
    EXPECT_TRUE(is_valid(TimeInForce::FillOrKill));
    EXPECT_FALSE(is_valid(static_cast<TimeInForce>(3)));
}

TEST(Status, Names) {
    EXPECT_TRUE(ok(Status::Ok));
    EXPECT_FALSE(ok(Status::Io));
    EXPECT_EQ(to_string(Status::Ok), "Ok");
    EXPECT_EQ(to_string(Status::WouldBlock), "WouldBlock");
    EXPECT_EQ(to_string(Status::Permission), "Permission");
    EXPECT_EQ(to_string(static_cast<Status>(200)), "Unknown");
}

}  // namespace
}  // namespace planck::core
