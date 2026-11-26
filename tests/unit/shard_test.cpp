#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "planck/engine/shard.hpp"
#include "support/shard_harness.hpp"

namespace planck::engine {
namespace {

using core::Side;
using core::Status;
using core::TimeInForce;
using test::as;
using test::cancel_order;
using test::new_order;
using test::small_shard;
using test::type_of;

// A shard driven by hand on the test's own thread: push one command, poll
// once, collect what came out.
class ShardTest : public ::testing::Test {
protected:
    void SetUp() override { start(small_shard()); }

    void start(const ShardConfig& config) {
        shard_ = std::make_unique<Shard>();
        ASSERT_EQ(shard_->init(config), Status::Ok);
    }

    void collect() {
        shard_->reports().drain(1U << 20, [this](const Report& r) noexcept { reports_.push_back(r); });
        shard_->market().drain(1U << 20, [this](const MarketEvent& e) noexcept { market_.push_back(e); });
    }

    void submit(const Command& command) {
        reports_.clear();
        market_.clear();
        ASSERT_TRUE(shard_->commands().try_push(command));
        ASSERT_EQ(shard_->poll(), 1U);
        collect();
    }

    // Rest an order and return the venue's id for it.
    std::uint64_t rest(std::uint16_t session, Side side, std::int32_t price, std::uint32_t qty,
                       std::uint16_t symbol = 3) {
        submit(new_order(session, symbol, side, price, qty, next_client_id_++));
        EXPECT_EQ(reports_.size(), 1U);
        EXPECT_EQ(market_.size(), 1U);
        return reports_.empty() ? 0 : as<wire::OrderAccepted>(reports_[0]).order_id;
    }

    std::unique_ptr<Shard> shard_;
    std::vector<Report> reports_;
    std::vector<MarketEvent> market_;
    std::uint64_t next_client_id_ = 1000;
};

TEST(ShardConfigTest, RejectsBadConfigurations) {
    const auto init_with = [](auto change) {
        ShardConfig config = small_shard();
        change(config);
        const auto shard = std::make_unique<Shard>();
        const Status status = shard->init(config);
        EXPECT_FALSE(shard->initialised());
        return status;
    };
    static constexpr SymbolConfig kDuplicate[] = {{core::SymbolId{3}, core::Price{0}, 64},
                                                  {core::SymbolId{3}, core::Price{0}, 64}};
    static constexpr SymbolConfig kBadId[] = {{core::SymbolId{5000}, core::Price{0}, 64}};
    static constexpr SymbolConfig kBadBand[] = {{core::SymbolId{5}, core::Price{0}, 100}};

    EXPECT_EQ(init_with([](ShardConfig& c) { c.symbols = {}; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](ShardConfig& c) { c.order_capacity = 0; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](ShardConfig& c) { c.command_ring_capacity = 1000; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](ShardConfig& c) { c.report_ring_capacity = 64; }), Status::InvalidArgument)
        << "report ring smaller than the stage";
    EXPECT_EQ(init_with([](ShardConfig& c) { c.shard = core::ShardId{16}; }), Status::OutOfRange);
    EXPECT_EQ(init_with([](ShardConfig& c) { c.symbols = kDuplicate; }), Status::Duplicate);
    EXPECT_EQ(init_with([](ShardConfig& c) { c.symbols = kBadId; }), Status::OutOfRange);
    EXPECT_EQ(init_with([](ShardConfig& c) { c.symbols = kBadBand; }), Status::InvalidArgument);
}

TEST_F(ShardTest, StartsIdleWithItsBooks) {
    EXPECT_TRUE(shard_->initialised());
    EXPECT_EQ(shard_->id(), core::ShardId{2});
    EXPECT_EQ(shard_->init(small_shard()), Status::Duplicate);
    EXPECT_NE(shard_->book(core::SymbolId{3}), nullptr);
    EXPECT_NE(shard_->book(core::SymbolId{700}), nullptr);
    EXPECT_EQ(shard_->book(core::SymbolId{4}), nullptr);
    EXPECT_EQ(shard_->book(core::SymbolId{9999}), nullptr);
    EXPECT_EQ(shard_->poll(), 0U);
}

TEST_F(ShardTest, UnknownSymbolIsRejectedToTheSender) {
    submit(new_order(1, 99, Side::Buy, 1000, 5, 11, TimeInForce::Day, core::OrderType::Limit, 12345));
    ASSERT_EQ(reports_.size(), 1U);
    EXPECT_TRUE(market_.empty());
    EXPECT_EQ(reports_[0].session, core::SessionId{1});
    EXPECT_EQ(reports_[0].ingress_tsc, 12345U) << "ingress time must ride along for latency measurement";
    const auto rejected = as<wire::OrderRejected>(reports_[0]);
    EXPECT_EQ(rejected.reason, wire::RejectCode::UnknownSymbol);
    EXPECT_EQ(rejected.client_order_id, 11U);
}

TEST_F(ShardTest, BookRejectionsBecomeWireCodes) {
    submit(new_order(1, 3, Side::Buy, 5000, 5, 1));
    EXPECT_EQ(as<wire::OrderRejected>(reports_.at(0)).reason, wire::RejectCode::PriceOutOfBand);
    submit(new_order(1, 3, Side::Buy, 1000, 0, 2));
    EXPECT_EQ(as<wire::OrderRejected>(reports_.at(0)).reason, wire::RejectCode::InvalidQuantity);
    submit(new_order(1, 3, Side::Sell, 1000, 9, 3, TimeInForce::FillOrKill));
    EXPECT_EQ(as<wire::OrderRejected>(reports_.at(0)).reason, wire::RejectCode::NotFillable);
    EXPECT_TRUE(market_.empty());
    EXPECT_EQ(shard_->stats().rejected, 3U);
}

TEST_F(ShardTest, RestingOrderIsAcknowledgedAndPublished) {
    submit(new_order(1, 3, Side::Buy, 1000, 10, 15));
    ASSERT_EQ(reports_.size(), 1U);
    ASSERT_EQ(market_.size(), 1U);

    const auto accepted = as<wire::OrderAccepted>(reports_[0]);
    EXPECT_EQ(accepted.client_order_id, 15U);
    EXPECT_NE(accepted.order_id, 0U);
    EXPECT_EQ(accepted.order_id >> 60, 2U) << "order id carries the shard";
    EXPECT_GT(accepted.timestamp, 1'600'000'000ULL * 1'000'000'000ULL) << "timestamp is wall-clock ns";

    const auto added = as<wire::MdAddOrder>(market_[0]);
    EXPECT_EQ(added.order_id, accepted.order_id);
    EXPECT_EQ(added.symbol_id, 3);
    EXPECT_EQ(added.side, 0);
    EXPECT_EQ(added.price, 1000);
    EXPECT_EQ(added.qty, 10U);
    EXPECT_EQ(added.timestamp, accepted.timestamp) << "one clock read per command";
}

TEST_F(ShardTest, TradeIsReportedToBothSidesAndPrintedOnce) {
    const std::uint64_t first = rest(1, Side::Buy, 1000, 10);
    const std::uint64_t second = rest(2, Side::Buy, 1000, 4);

    // Session 3 sells 12: all of the first order and half of the second.
    submit(new_order(3, 3, Side::Sell, 999, 12, 17));
    ASSERT_EQ(reports_.size(), 5U);
    ASSERT_EQ(market_.size(), 2U);

    const auto accepted = as<wire::OrderAccepted>(reports_[0]);
    EXPECT_EQ(reports_[0].session, core::SessionId{3}) << "acceptance comes before any fill";

    const auto taker1 = as<wire::Executed>(reports_[1]);
    const auto maker1 = as<wire::Executed>(reports_[2]);
    const auto taker2 = as<wire::Executed>(reports_[3]);
    const auto maker2 = as<wire::Executed>(reports_[4]);
    EXPECT_EQ(reports_[1].session, core::SessionId{3});
    EXPECT_EQ(reports_[2].session, core::SessionId{1});
    EXPECT_EQ(reports_[3].session, core::SessionId{3});
    EXPECT_EQ(reports_[4].session, core::SessionId{2});

    EXPECT_EQ(taker1.liquidity, wire::Liquidity::Taker);
    EXPECT_EQ(taker1.order_id, accepted.order_id);
    EXPECT_EQ(taker1.qty, 10U);
    EXPECT_EQ(taker1.price, 1000) << "trade at the resting price, not the taker's 999";
    EXPECT_EQ(taker1.leaves, 2U);
    EXPECT_EQ(maker1.liquidity, wire::Liquidity::Maker);
    EXPECT_EQ(maker1.order_id, first);
    EXPECT_EQ(maker1.leaves, 0U);
    EXPECT_EQ(maker1.match_id, taker1.match_id);
    EXPECT_EQ(maker1.match_id >> 60, 2U);

    EXPECT_EQ(taker2.qty, 2U);
    EXPECT_EQ(taker2.leaves, 0U);
    EXPECT_EQ(maker2.order_id, second);
    EXPECT_EQ(maker2.leaves, 2U);
    EXPECT_EQ(taker2.match_id, taker1.match_id + 1);

    const auto print1 = as<wire::MdExecuted>(market_[0]);
    const auto print2 = as<wire::MdExecuted>(market_[1]);
    EXPECT_EQ(print1.order_id, first);
    EXPECT_EQ(print1.qty, 10U);
    EXPECT_EQ(print1.price, 1000);
    EXPECT_EQ(print1.match_id, taker1.match_id);
    EXPECT_EQ(print2.order_id, second);
    EXPECT_EQ(print2.qty, 2U);
    EXPECT_EQ(shard_->stats().trades, 2U);
}

TEST_F(ShardTest, OwnerCanCancel) {
    const std::uint64_t id = rest(5, Side::Sell, -100, 3, 700);
    submit(cancel_order(5, id));
    ASSERT_EQ(reports_.size(), 1U);
    ASSERT_EQ(market_.size(), 1U);

    const auto canceled = as<wire::Canceled>(reports_[0]);
    EXPECT_EQ(canceled.reason, wire::CancelReason::UserRequested);
    EXPECT_EQ(canceled.order_id, id);
    EXPECT_EQ(canceled.qty, 3U);
    const auto deleted = as<wire::MdDeleted>(market_[0]);
    EXPECT_EQ(deleted.order_id, id);
    EXPECT_EQ(deleted.symbol_id, 700);
    EXPECT_EQ(shard_->store().in_use(), 0U);
}

TEST_F(ShardTest, NobodyElseCanCancel) {
    const std::uint64_t id = rest(1, Side::Buy, 1000, 10);

    submit(cancel_order(2, id));
    ASSERT_EQ(reports_.size(), 1U);
    EXPECT_TRUE(market_.empty());
    const auto refused = as<wire::CancelRejected>(reports_[0]);
    // Deliberately the same answer as for an id that never existed.
    EXPECT_EQ(refused.reason, wire::RejectCode::UnknownOrder);
    EXPECT_EQ(refused.order_id, id);
    EXPECT_EQ(shard_->store().in_use(), 1U) << "the order must still be resting";

    submit(cancel_order(2, 123456));
    EXPECT_EQ(as<wire::CancelRejected>(reports_.at(0)).reason, wire::RejectCode::UnknownOrder);
}

TEST_F(ShardTest, FilledOrderCannotBeCancelled) {
    const std::uint64_t id = rest(1, Side::Buy, 1000, 10);
    submit(new_order(2, 3, Side::Sell, 1000, 10, 50));
    submit(cancel_order(1, id));
    ASSERT_EQ(reports_.size(), 1U);
    EXPECT_EQ(type_of(reports_[0]), wire::MsgType::CancelRejected);
}

TEST_F(ShardTest, UnfilledRemainderOfAnIocIsCancelled) {
    rest(1, Side::Buy, 1000, 2);
    submit(new_order(4, 3, Side::Sell, 990, 50, 18, TimeInForce::ImmediateOrCancel));
    ASSERT_EQ(reports_.size(), 4U);
    EXPECT_EQ(type_of(reports_[0]), wire::MsgType::OrderAccepted);
    EXPECT_EQ(type_of(reports_[1]), wire::MsgType::Executed);
    EXPECT_EQ(type_of(reports_[2]), wire::MsgType::Executed);

    const auto canceled = as<wire::Canceled>(reports_[3]);
    EXPECT_EQ(reports_[3].session, core::SessionId{4});
    EXPECT_EQ(canceled.reason, wire::CancelReason::Unfilled);
    EXPECT_EQ(canceled.qty, 48U);
    EXPECT_EQ(canceled.order_id, as<wire::OrderAccepted>(reports_[0]).order_id);
    ASSERT_EQ(market_.size(), 1U) << "the remainder never rested, so no add";
    EXPECT_EQ(type_of(market_[0]), wire::MsgType::MdExecuted);
}

TEST_F(ShardTest, MarketOrderIntoAnEmptyBookIsAcceptedThenCancelled) {
    submit(new_order(4, 3, Side::Buy, 0, 7, 19, TimeInForce::Day, core::OrderType::Market));
    ASSERT_EQ(reports_.size(), 2U);
    EXPECT_TRUE(market_.empty());
    EXPECT_EQ(type_of(reports_[0]), wire::MsgType::OrderAccepted);
    EXPECT_EQ(as<wire::Canceled>(reports_[1]).qty, 7U);
}

TEST_F(ShardTest, SweepLargerThanTheStageLosesNothing) {
    // 300 fills make 601 reports, well past the 128-entry stage.
    constexpr std::uint32_t kMakers = 300;
    for (std::uint32_t i = 0; i < kMakers; ++i) {
        rest(static_cast<std::uint16_t>(10 + i % 7), Side::Sell, static_cast<std::int32_t>(1010 + i % 3), 1);
    }
    submit(new_order(9, 3, Side::Buy, 1100, kMakers, 5000));
    ASSERT_EQ(reports_.size(), 1U + 2U * kMakers);
    ASSERT_EQ(market_.size(), kMakers);
    EXPECT_EQ(type_of(reports_[0]), wire::MsgType::OrderAccepted);

    std::uint32_t left = kMakers;
    std::int32_t last_price = 0;
    for (std::uint32_t i = 0; i < kMakers; ++i) {
        const auto taker = as<wire::Executed>(reports_[1 + 2 * i]);
        const auto maker = as<wire::Executed>(reports_[2 + 2 * i]);
        --left;
        ASSERT_EQ(taker.leaves, left);
        ASSERT_EQ(taker.liquidity, wire::Liquidity::Taker);
        ASSERT_EQ(maker.liquidity, wire::Liquidity::Maker);
        ASSERT_EQ(maker.match_id, taker.match_id);
        ASSERT_GE(taker.price, last_price) << "a buy sweeps from the lowest ask upward";
        last_price = taker.price;
    }
    EXPECT_EQ(shard_->store().in_use(), 0U);
}

TEST_F(ShardTest, SlowConsumerStallsMatchingAndLosesNothing) {
    ShardConfig config = small_shard();
    config.report_ring_capacity = 128;
    config.market_ring_capacity = 128;
    start(config);

    constexpr std::uint64_t kOrders = 2000;
    for (std::uint64_t i = 0; i < kOrders; ++i) {
        const bool buy = (i % 2 == 1);
        ASSERT_TRUE(shard_->commands().try_push(
            new_order(1, 3, buy ? Side::Buy : Side::Sell, buy ? 950 : 1050, 1, i)));
    }

    // Nobody is reading the reports, so the shard must stop after the ring
    // fills and stay stopped.
    std::size_t handled = 0;
    for (std::size_t n = shard_->poll(); n != 0; n = shard_->poll()) {
        handled += n;
    }
    EXPECT_GE(handled, 128U);
    EXPECT_LE(handled, 130U);
    EXPECT_EQ(shard_->reports().size_approx(), 128U);
    EXPECT_GE(shard_->stats().stalls, 1U);
    EXPECT_EQ(shard_->poll(), 0U);

    // Start reading. Everything arrives, in order.
    for (int round = 0; round < 100'000 && handled < kOrders; ++round) {
        collect();
        handled += shard_->poll();
    }
    collect();
    static_cast<void>(shard_->poll());
    collect();
    ASSERT_EQ(handled, kOrders);
    ASSERT_EQ(reports_.size(), kOrders);
    ASSERT_EQ(market_.size(), kOrders);
    for (std::uint64_t i = 0; i < kOrders; ++i) {
        ASSERT_EQ(as<wire::OrderAccepted>(reports_[i]).client_order_id, i);
    }
}

TEST_F(ShardTest, TimestampsNeverGoBackwards) {
    // The shard re-anchors its clock to the wall clock when idle, and the
    // wall clock can be steered backwards. Message timestamps must not follow.
    std::uint64_t previous = 0;
    for (int round = 0; round < 200; ++round) {
        submit(new_order(1, 3, Side::Buy, 950, 1, next_client_id_++, TimeInForce::ImmediateOrCancel));
        const std::uint64_t timestamp = as<wire::OrderAccepted>(reports_.at(0)).timestamp;
        ASSERT_GE(timestamp, previous);
        previous = timestamp;
        static_cast<void>(shard_->poll());  // an idle pass, which may re-anchor
    }
}

TEST_F(ShardTest, CountsWhatItDid) {
    const std::uint64_t id = rest(1, Side::Buy, 1000, 10);
    submit(new_order(2, 3, Side::Sell, 1000, 4, 1));
    submit(new_order(2, 99, Side::Sell, 1000, 4, 2));
    submit(cancel_order(1, id));
    submit(cancel_order(1, id));

    const ShardStats& stats = shard_->stats();
    EXPECT_EQ(stats.commands, 5U);
    EXPECT_EQ(stats.accepted, 2U);
    EXPECT_EQ(stats.rejected, 1U);
    EXPECT_EQ(stats.trades, 1U);
    EXPECT_EQ(stats.cancels, 1U);
    EXPECT_EQ(stats.cancel_rejects, 1U);
    EXPECT_EQ(stats.stalls, 0U);
}

}  // namespace
}  // namespace planck::engine
