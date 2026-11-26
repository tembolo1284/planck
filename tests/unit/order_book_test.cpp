#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "planck/book/order_book.hpp"
#include "planck/mem/arena.hpp"
#include "support/fixtures.hpp"

namespace planck::book {
namespace {

using core::OrderId;
using core::OrderType;
using core::Price;
using core::Qty;
using core::Side;
using core::Status;
using core::TimeInForce;

// One book for symbol 7 with prices 100..163, on a store with room for 8
// resting orders in shard 5.
class OrderBookTest : public ::testing::Test {
protected:
    static constexpr std::uint32_t kCapacity = 8;

    void SetUp() override {
        ASSERT_EQ(arena_.init(4 << 20, sys::HugePages::Off), Status::Ok);
        ASSERT_EQ(store_.init(arena_.allocate_array<Order>(kCapacity), core::ShardId{5}), Status::Ok);
        ASSERT_EQ(book_.init({core::SymbolId{7}, Price{100}, 64}, store_, arena_), Status::Ok);
    }

    AddResult limit(Side side, std::int32_t price, std::uint32_t qty,
                    TimeInForce tif = TimeInForce::Day, std::uint32_t owner = 0) {
        return book_.add({side, OrderType::Limit, tif, Price{price}, Qty{qty}, owner}, log_);
    }

    AddResult market(Side side, std::uint32_t qty, TimeInForce tif = TimeInForce::ImmediateOrCancel) {
        return book_.add({side, OrderType::Market, tif, Price{0}, Qty{qty}, 0}, log_);
    }

    // Rest an order and return its id. Fails the test if it does not rest.
    OrderId resting(Side side, std::int32_t price, std::uint32_t qty, std::uint32_t owner = 0) {
        const AddResult result = limit(side, price, qty, TimeInForce::Day, owner);
        EXPECT_EQ(result.reject, RejectReason::None);
        EXPECT_EQ(result.resting, Qty{qty});
        return result.id;
    }

    mem::Arena arena_;
    OrderStore store_;
    OrderBook book_;
    test::TradeLog log_;
};

TEST_F(OrderBookTest, InitValidation) {
    OrderBook book;
    EXPECT_EQ(book.init({core::SymbolId{1}, Price{0}, 100}, store_, arena_), Status::InvalidArgument)
        << "band must be a power of two";
    EXPECT_EQ(book.init({core::SymbolId{1}, Price{0}, 32}, store_, arena_), Status::InvalidArgument)
        << "band below the minimum";
    const Price near_top{std::numeric_limits<std::int32_t>::max() - 10};
    EXPECT_EQ(book.init({core::SymbolId{1}, near_top, 64}, store_, arena_), Status::OutOfRange)
        << "band would run past the largest price";
    EXPECT_FALSE(book.initialised());
    EXPECT_EQ(book_.init({core::SymbolId{7}, Price{100}, 64}, store_, arena_), Status::Duplicate);
}

TEST_F(OrderBookTest, StartsEmpty) {
    EXPECT_FALSE(book_.best(Side::Buy).valid);
    EXPECT_FALSE(book_.best(Side::Sell).valid);
    EXPECT_EQ(book_.quantity_at(Price{120}), 0U);
    EXPECT_TRUE(book_.in_band(Price{100}));
    EXPECT_TRUE(book_.in_band(Price{163}));
    EXPECT_FALSE(book_.in_band(Price{99}));
    EXPECT_FALSE(book_.in_band(Price{164}));
}

TEST_F(OrderBookTest, RejectsBadOrdersWithoutSideEffects) {
    EXPECT_EQ(limit(Side::Buy, 120, 0).reject, RejectReason::InvalidQuantity);
    EXPECT_EQ(limit(Side::Buy, 164, 5).reject, RejectReason::PriceOutOfBand);
    EXPECT_EQ(limit(Side::Sell, 99, 5).reject, RejectReason::PriceOutOfBand);
    EXPECT_EQ(limit(Side::Buy, std::numeric_limits<std::int32_t>::max(), 5).reject,
              RejectReason::PriceOutOfBand);
    EXPECT_EQ(limit(Side::Buy, std::numeric_limits<std::int32_t>::min(), 5).reject,
              RejectReason::PriceOutOfBand);
    EXPECT_EQ(limit(Side::Buy, 120, 0).id, core::kNoOrderId) << "a rejected order gets no id";
    EXPECT_EQ(store_.in_use(), 0U);
    EXPECT_TRUE(log_.trades.empty());
}

TEST_F(OrderBookTest, RestingOrdersFormTheQuote) {
    resting(Side::Buy, 118, 10);
    resting(Side::Buy, 120, 5);
    resting(Side::Buy, 120, 7);
    resting(Side::Sell, 125, 3);
    resting(Side::Sell, 130, 9);

    const Quote bid = book_.best(Side::Buy);
    const Quote ask = book_.best(Side::Sell);
    ASSERT_TRUE(bid.valid);
    ASSERT_TRUE(ask.valid);
    EXPECT_EQ(bid.price, Price{120});
    EXPECT_EQ(bid.qty, 12U);
    EXPECT_EQ(ask.price, Price{125});
    EXPECT_EQ(ask.qty, 3U);
    EXPECT_EQ(book_.quantity_at(Price{118}), 10U);
    EXPECT_EQ(book_.quantity_at(Price{130}), 9U);
    EXPECT_EQ(book_.quantity_at(Price{500}), 0U);
    EXPECT_EQ(store_.in_use(), 5U);
}

TEST_F(OrderBookTest, IdsCarryTheShardAndNeverRepeat) {
    const OrderId first = resting(Side::Buy, 110, 1);
    const OrderId second = resting(Side::Buy, 110, 1);
    EXPECT_NE(first, core::kNoOrderId);
    EXPECT_NE(first, second);
    EXPECT_EQ(OrderStore::shard_of(first), core::ShardId{5});

    // Cancel and re-enter: the slot is reused but the id is new.
    ASSERT_EQ(book_.cancel(second).reject, RejectReason::None);
    const OrderId third = resting(Side::Buy, 110, 1);
    EXPECT_NE(third, second);
    EXPECT_EQ(book_.cancel(second).reject, RejectReason::UnknownOrder) << "stale id must not resolve";
}

TEST_F(OrderBookTest, TimePriorityWithinALevel) {
    const OrderId first = resting(Side::Sell, 125, 4);
    const OrderId second = resting(Side::Sell, 125, 4);
    const OrderId third = resting(Side::Sell, 125, 4);

    const AddResult taker = limit(Side::Buy, 125, 6);
    EXPECT_EQ(taker.filled, Qty{6});
    EXPECT_EQ(taker.resting, Qty{0});
    ASSERT_EQ(log_.trades.size(), 2U);
    EXPECT_EQ(log_.trades[0].resting, first);
    EXPECT_EQ(log_.trades[0].qty, Qty{4});
    EXPECT_EQ(log_.trades[0].resting_leaves, Qty{0});
    EXPECT_EQ(log_.trades[1].resting, second);
    EXPECT_EQ(log_.trades[1].qty, Qty{2});
    EXPECT_EQ(log_.trades[1].resting_leaves, Qty{2});

    // The partly filled order keeps its place ahead of the third.
    log_.trades.clear();
    static_cast<void>(limit(Side::Buy, 125, 3));
    ASSERT_EQ(log_.trades.size(), 2U);
    EXPECT_EQ(log_.trades[0].resting, second);
    EXPECT_EQ(log_.trades[1].resting, third);
}

TEST_F(OrderBookTest, BetterPriceTradesFirstAtTheRestingPrice) {
    const OrderId worse = resting(Side::Sell, 126, 5);
    const OrderId better = resting(Side::Sell, 124, 5);

    const AddResult taker = limit(Side::Buy, 130, 8);
    EXPECT_EQ(taker.filled, Qty{8});
    ASSERT_EQ(log_.trades.size(), 2U);
    EXPECT_EQ(log_.trades[0].resting, better);
    EXPECT_EQ(log_.trades[0].price, Price{124}) << "price improvement goes to the taker";
    EXPECT_EQ(log_.trades[1].resting, worse);
    EXPECT_EQ(log_.trades[1].price, Price{126});
    EXPECT_EQ(log_.trades[1].aggressor, taker.id);
    EXPECT_EQ(log_.trades[1].aggressor_side, Side::Buy);
    EXPECT_EQ(book_.best(Side::Sell).qty, 2U);
}

TEST_F(OrderBookTest, DoesNotTradeThroughItsLimit) {
    resting(Side::Sell, 124, 5);
    resting(Side::Sell, 126, 5);
    const AddResult taker = limit(Side::Buy, 125, 20);
    EXPECT_EQ(taker.filled, Qty{5});
    EXPECT_EQ(taker.resting, Qty{15}) << "the rest waits at 125";
    EXPECT_EQ(book_.best(Side::Buy).price, Price{125});
    EXPECT_EQ(book_.best(Side::Sell).price, Price{126});
}

TEST_F(OrderBookTest, ImmediateOrCancelNeverRests) {
    resting(Side::Buy, 120, 5);
    const AddResult taker = limit(Side::Sell, 119, 50, TimeInForce::ImmediateOrCancel);
    EXPECT_EQ(taker.reject, RejectReason::None);
    EXPECT_EQ(taker.filled, Qty{5});
    EXPECT_EQ(taker.resting, Qty{0});
    EXPECT_NE(taker.id, core::kNoOrderId);
    EXPECT_FALSE(book_.best(Side::Sell).valid);
    EXPECT_EQ(book_.cancel(taker.id).reject, RejectReason::UnknownOrder);
    EXPECT_EQ(store_.in_use(), 0U);
}

TEST_F(OrderBookTest, FillOrKillIsAllOrNothing) {
    resting(Side::Buy, 120, 5);
    resting(Side::Buy, 118, 5);

    EXPECT_EQ(limit(Side::Sell, 120, 6, TimeInForce::FillOrKill).reject, RejectReason::NotFillable)
        << "only 5 available at 120 or better";
    EXPECT_TRUE(log_.trades.empty());
    EXPECT_EQ(book_.best(Side::Buy).qty, 5U);

    const AddResult both = limit(Side::Sell, 118, 10, TimeInForce::FillOrKill);
    EXPECT_EQ(both.reject, RejectReason::None);
    EXPECT_EQ(both.filled, Qty{10});
    EXPECT_FALSE(book_.best(Side::Buy).valid);
}

TEST_F(OrderBookTest, MarketOrderSweepsAndNeverRests) {
    resting(Side::Sell, 124, 5);
    resting(Side::Sell, 160, 5);
    const AddResult sweep = market(Side::Buy, 50);
    EXPECT_EQ(sweep.filled, Qty{10});
    EXPECT_EQ(sweep.resting, Qty{0});
    EXPECT_FALSE(book_.best(Side::Sell).valid);
    EXPECT_FALSE(book_.best(Side::Buy).valid);

    // Into an empty book it is accepted and does nothing, even marked Day.
    const AddResult nothing = market(Side::Sell, 5, TimeInForce::Day);
    EXPECT_EQ(nothing.reject, RejectReason::None);
    EXPECT_EQ(nothing.filled, Qty{0});
    EXPECT_EQ(nothing.resting, Qty{0});
}

TEST_F(OrderBookTest, CancelFromHeadMiddleAndTail) {
    const OrderId head = resting(Side::Buy, 120, 1);
    const OrderId middle = resting(Side::Buy, 120, 2);
    const OrderId tail = resting(Side::Buy, 120, 4);
    const OrderId lone = resting(Side::Buy, 115, 8);

    const CancelResult cancelled = book_.cancel(middle);
    EXPECT_EQ(cancelled.reject, RejectReason::None);
    EXPECT_EQ(cancelled.cancelled, Qty{2});
    EXPECT_EQ(cancelled.price, Price{120});
    EXPECT_EQ(cancelled.side, Side::Buy);
    EXPECT_EQ(book_.best(Side::Buy).qty, 5U);

    ASSERT_EQ(book_.cancel(head).reject, RejectReason::None);
    EXPECT_EQ(book_.best(Side::Buy).qty, 4U);
    ASSERT_EQ(book_.cancel(tail).reject, RejectReason::None);
    EXPECT_EQ(book_.best(Side::Buy).price, Price{115}) << "best moves down when its level empties";
    ASSERT_EQ(book_.cancel(lone).reject, RejectReason::None);
    EXPECT_FALSE(book_.best(Side::Buy).valid);
    EXPECT_EQ(store_.in_use(), 0U);
}

TEST_F(OrderBookTest, QueueSurvivesACancelInTheMiddle) {
    const OrderId first = resting(Side::Sell, 125, 1);
    const OrderId second = resting(Side::Sell, 125, 1);
    const OrderId third = resting(Side::Sell, 125, 1);
    ASSERT_EQ(book_.cancel(second).reject, RejectReason::None);

    static_cast<void>(limit(Side::Buy, 125, 2));
    ASSERT_EQ(log_.trades.size(), 2U);
    EXPECT_EQ(log_.trades[0].resting, first);
    EXPECT_EQ(log_.trades[1].resting, third);
}

TEST_F(OrderBookTest, CancelOfUnknownIdsIsHarmless) {
    const OrderId id = resting(Side::Buy, 120, 1);
    for (const std::uint64_t raw : {std::uint64_t{0}, std::uint64_t{1}, ~std::uint64_t{0}, id.raw() + 1,
                                    id.raw() ^ (std::uint64_t{1} << 40)}) {
        EXPECT_EQ(book_.cancel(OrderId{raw}).reject, RejectReason::UnknownOrder) << raw;
    }
    EXPECT_EQ(book_.best(Side::Buy).qty, 1U);
    ASSERT_EQ(book_.cancel(id).reject, RejectReason::None);
    EXPECT_EQ(book_.cancel(id).reject, RejectReason::UnknownOrder) << "second cancel of the same id";
}

TEST_F(OrderBookTest, FullStoreRejectsRestingOrdersButStillLetsTakersIn) {
    for (std::uint32_t i = 0; i < kCapacity; ++i) {
        resting(Side::Buy, 110, 1);
    }
    EXPECT_EQ(limit(Side::Buy, 111, 1).reject, RejectReason::BookFull);
    EXPECT_EQ(limit(Side::Sell, 150, 1).reject, RejectReason::BookFull);

    // An IOC needs no slot, so it can still trade and free one up.
    const AddResult taker = limit(Side::Sell, 110, 1, TimeInForce::ImmediateOrCancel);
    EXPECT_EQ(taker.reject, RejectReason::None);
    EXPECT_EQ(taker.filled, Qty{1});
    EXPECT_EQ(limit(Side::Buy, 111, 1).reject, RejectReason::None);
}

TEST_F(OrderBookTest, OwnerTagComesBackOnTheFill) {
    resting(Side::Sell, 125, 5, 4242);
    static_cast<void>(limit(Side::Buy, 125, 5, TimeInForce::Day, 99));
    ASSERT_EQ(log_.trades.size(), 1U);
    EXPECT_EQ(log_.trades[0].resting_owner, 4242U);
}

TEST_F(OrderBookTest, BooksSharingAStoreStaySeparate) {
    OrderBook other;
    ASSERT_EQ(other.init({core::SymbolId{8}, Price{-32}, 64}, store_, arena_), Status::Ok);

    const OrderId here = resting(Side::Buy, 120, 5);
    const AddResult there =
        other.add({Side::Sell, OrderType::Limit, TimeInForce::Day, Price{-3}, Qty{2}, 0}, log_);
    ASSERT_EQ(there.reject, RejectReason::None);
    EXPECT_EQ(other.best(Side::Sell).price, Price{-3}) << "negative prices are ordinary prices";
    EXPECT_TRUE(log_.trades.empty()) << "different symbols never match";

    // A live id is only cancellable through the book that holds it.
    EXPECT_EQ(other.cancel(here).reject, RejectReason::UnknownOrder);
    EXPECT_EQ(book_.cancel(there.id).reject, RejectReason::UnknownOrder);
    EXPECT_EQ(book_.cancel(here).reject, RejectReason::None);
    EXPECT_EQ(other.cancel(there.id).reject, RejectReason::None);
}

TEST(RejectReasonNames, AreStable) {
    EXPECT_EQ(to_string(RejectReason::None), "None");
    EXPECT_EQ(to_string(RejectReason::BookFull), "BookFull");
    EXPECT_EQ(to_string(static_cast<RejectReason>(99)), "Unknown");
}

}  // namespace
}  // namespace planck::book
