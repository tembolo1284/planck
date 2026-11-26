#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>

#include "planck/engine/shard.hpp"
#include "planck/gateway/gateway.hpp"
#include "support/wire_client.hpp"

namespace planck::gateway {
namespace {

using core::Status;
using test::WireClient;

constexpr engine::SymbolConfig kEquities[] = {{core::SymbolId{3}, core::Price{900}, 256}};
constexpr engine::SymbolConfig kSpreads[] = {{core::SymbolId{700}, core::Price{-128}, 256}};
constexpr Route kRoutes[] = {{core::SymbolId{3}, 0}, {core::SymbolId{700}, 1}};

engine::ShardConfig shard_config(std::uint8_t id, std::span<const engine::SymbolConfig> symbols) {
    engine::ShardConfig config;
    config.shard = core::ShardId{id};
    config.symbols = symbols;
    config.order_capacity = 1024;
    config.command_ring_capacity = 1U << 10;
    config.report_ring_capacity = 1U << 10;
    config.market_ring_capacity = 1U << 10;
    config.huge_pages = sys::HugePages::Off;
    config.pin = false;
    config.idle = engine::IdlePolicy::Yield;
    return config;
}

wire::NewOrder limit(std::uint16_t symbol, core::Side side, std::int32_t price, std::uint32_t qty,
                     std::uint64_t client_order_id) {
    wire::NewOrder order{};
    order.symbol_id = symbol;
    order.side = std::to_underlying(side);
    order.price = price;
    order.qty = qty;
    order.client_order_id = client_order_id;
    return order;
}

// Two shards and a gateway, all driven by hand on the test's own thread.
// Shard 2 trades symbol 3. Shard 5 trades symbol 700. Real TCP on loopback.
class GatewayTest : public ::testing::Test {
protected:
    void SetUp() override { start({}); }

    void start(GatewayConfig config, std::uint32_t command_ring = 1U << 10) {
        for (auto& shard : shards_) {
            shard = std::make_unique<engine::Shard>();
        }
        engine::ShardConfig first = shard_config(2, kEquities);
        first.command_ring_capacity = command_ring;
        ASSERT_EQ(shards_[0]->init(first), Status::Ok);
        ASSERT_EQ(shards_[1]->init(shard_config(5, kSpreads)), Status::Ok);
        pointers_ = {shards_[0].get(), shards_[1].get()};

        config.listen = {net::kLoopback, 0};
        config.shards = pointers_;
        config.routes = kRoutes;
        config.idle = engine::IdlePolicy::Yield;
        gateway_ = std::make_unique<Gateway>();
        ASSERT_EQ(gateway_->init(config), Status::Ok);
        ASSERT_EQ(gateway_->local_endpoint(server_), Status::Ok);
    }

    void pump() {
        static_cast<void>(gateway_->poll());
        if (shards_running_) {
            for (auto& shard : shards_) {
                static_cast<void>(shard->poll());
            }
        }
        static_cast<void>(gateway_->poll());
    }

    // A connected client that drives the server while it waits.
    std::unique_ptr<WireClient> client() {
        auto made = std::make_unique<WireClient>([this] { pump(); });
        made->connect(server_);
        return made;
    }

    std::array<std::unique_ptr<engine::Shard>, 2> shards_;
    std::array<engine::Shard*, 2> pointers_{};
    std::unique_ptr<Gateway> gateway_;
    net::Endpoint server_{};
    bool shards_running_ = true;
};

TEST(GatewayConfigTest, RejectsBadConfigurations) {
    auto shard = std::make_unique<engine::Shard>();
    ASSERT_EQ(shard->init(shard_config(2, kEquities)), Status::Ok);
    engine::Shard* const one[] = {shard.get()};
    engine::Shard* const twice[] = {shard.get(), shard.get()};
    engine::Shard never_started;
    engine::Shard* const cold[] = {&never_started};
    static constexpr Route kWrongShard[] = {{core::SymbolId{3}, 4}};
    static constexpr Route kNotTraded[] = {{core::SymbolId{9}, 0}};

    const auto init_with = [&](auto change) {
        GatewayConfig config;
        config.listen = {net::kLoopback, 0};
        config.shards = one;
        change(config);
        Gateway gateway;
        const Status status = gateway.init(config);
        EXPECT_FALSE(gateway.initialised());
        return status;
    };
    EXPECT_EQ(init_with([](GatewayConfig& c) { c.shards = {}; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](GatewayConfig& c) { c.max_sessions = 0; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](GatewayConfig& c) { c.tx_buffer_bytes = 8; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([&](GatewayConfig& c) { c.shards = twice; }), Status::Duplicate);
    EXPECT_EQ(init_with([&](GatewayConfig& c) { c.shards = cold; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](GatewayConfig& c) { c.routes = kWrongShard; }), Status::OutOfRange);
    EXPECT_EQ(init_with([](GatewayConfig& c) { c.routes = kNotTraded; }), Status::NotFound)
        << "a route to a shard that does not trade the symbol";
}

TEST_F(GatewayTest, LoginThenOrderIsAcknowledged) {
    const auto alice = client();
    const std::uint16_t session = alice->login(1001);
    EXPECT_NE(session, 0);
    EXPECT_EQ(gateway_->active_sessions(), 1U);

    alice->send(limit(3, core::Side::Buy, 1000, 10, 77));
    const auto accepted = alice->expect<wire::OrderAccepted>();
    EXPECT_EQ(accepted.client_order_id, 77U);
    EXPECT_EQ(accepted.order_id >> 60, 2U) << "symbol 3 belongs to shard 2";
    EXPECT_TRUE(alice->quiet());
    EXPECT_EQ(gateway_->stats().logins, 1U);
    EXPECT_EQ(gateway_->stats().commands_out, 1U);
}

TEST_F(GatewayTest, RequestsBeforeLoginAreRefused) {
    const auto stranger = client();
    stranger->send(limit(3, core::Side::Buy, 1000, 10, 5));
    const auto rejected = stranger->expect<wire::OrderRejected>();
    EXPECT_EQ(rejected.reason, wire::RejectCode::NotLoggedIn);
    EXPECT_EQ(rejected.client_order_id, 5U);

    wire::CancelOrder cancel{};
    cancel.order_id = 123;
    stranger->send(cancel);
    EXPECT_EQ(stranger->expect<wire::CancelRejected>().reason, wire::RejectCode::NotLoggedIn);
    EXPECT_EQ(gateway_->stats().commands_out, 0U) << "nothing may reach a shard before login";
}

TEST_F(GatewayTest, TwoClientsTradeAndEachHearsItsOwnSide) {
    const auto maker = client();
    const auto taker = client();
    maker->login(1);
    taker->login(2);

    maker->send(limit(3, core::Side::Sell, 1000, 10, 11));
    const auto resting = maker->expect<wire::OrderAccepted>();

    taker->send(limit(3, core::Side::Buy, 1005, 4, 22));
    const auto crossing = taker->expect<wire::OrderAccepted>();
    const auto taker_fill = taker->expect<wire::Executed>();
    const auto maker_fill = maker->expect<wire::Executed>();

    EXPECT_EQ(taker_fill.liquidity, wire::Liquidity::Taker);
    EXPECT_EQ(taker_fill.order_id, crossing.order_id);
    EXPECT_EQ(taker_fill.qty, 4U);
    EXPECT_EQ(taker_fill.price, 1000);
    EXPECT_EQ(taker_fill.leaves, 0U);
    EXPECT_EQ(maker_fill.liquidity, wire::Liquidity::Maker);
    EXPECT_EQ(maker_fill.order_id, resting.order_id);
    EXPECT_EQ(maker_fill.leaves, 6U);
    EXPECT_EQ(maker_fill.match_id, taker_fill.match_id);
    EXPECT_TRUE(maker->quiet());
    EXPECT_TRUE(taker->quiet());
}

TEST_F(GatewayTest, RoutesBySymbolAndCancelsByOrderId) {
    const auto alice = client();
    alice->login(1);

    alice->send(limit(700, core::Side::Sell, -50, 3, 1));
    const auto accepted = alice->expect<wire::OrderAccepted>();
    EXPECT_EQ(accepted.order_id >> 60, 5U) << "symbol 700 belongs to shard 5";

    // The cancel names no symbol. The order id alone finds the right shard.
    wire::CancelOrder cancel{};
    cancel.order_id = accepted.order_id;
    alice->send(cancel);
    const auto canceled = alice->expect<wire::Canceled>();
    EXPECT_EQ(canceled.order_id, accepted.order_id);
    EXPECT_EQ(canceled.qty, 3U);
    EXPECT_EQ(canceled.reason, wire::CancelReason::UserRequested);
}

TEST_F(GatewayTest, BadFieldsAreRejectedAtTheDoor) {
    const auto alice = client();
    alice->login(1);

    wire::NewOrder bad_side = limit(3, core::Side::Buy, 1000, 1, 1);
    bad_side.side = 9;
    alice->send(bad_side);
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::Malformed);

    wire::NewOrder bad_reserved = limit(3, core::Side::Buy, 1000, 1, 2);
    bad_reserved.reserved[3] = 1;
    alice->send(bad_reserved);
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::Malformed);

    alice->send(limit(4000, core::Side::Buy, 1000, 1, 3));
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::UnknownSymbol);
    alice->send(limit(60000, core::Side::Buy, 1000, 1, 4));
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::UnknownSymbol);

    wire::CancelOrder no_such_shard{};
    no_such_shard.order_id = std::uint64_t{9} << 60;
    alice->send(no_such_shard);
    EXPECT_EQ(alice->expect<wire::CancelRejected>().reason, wire::RejectCode::UnknownOrder);

    EXPECT_EQ(gateway_->stats().commands_out, 0U) << "none of these may reach a shard";
    EXPECT_EQ(gateway_->stats().rejected, 5U);
    EXPECT_EQ(gateway_->active_sessions(), 1U) << "a bad field is a reject, not a disconnect";
}

TEST_F(GatewayTest, BookRejectionsComeBackThroughTheShard) {
    const auto alice = client();
    alice->login(1);
    alice->send(limit(3, core::Side::Buy, 5000, 1, 1));
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::PriceOutOfBand);
    alice->send(limit(3, core::Side::Buy, 1000, 0, 2));
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::InvalidQuantity);
}

TEST_F(GatewayTest, MessageSplitAcrossPacketsIsReassembled) {
    const auto alice = client();
    alice->login(1);

    const wire::NewOrder order = limit(3, core::Side::Buy, 1000, 10, 9);
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(&order), sizeof(order)};
    alice->send_bytes(bytes.first(5));
    for (int i = 0; i < 20; ++i) {
        pump();
    }
    EXPECT_EQ(gateway_->stats().commands_out, 0U) << "acted on half a message";
    alice->send_bytes(bytes.subspan(5));
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 9U);
}

TEST_F(GatewayTest, ReadEndingMidMessageKeepsTheTail) {
    const auto alice = client();
    alice->login(1);

    // Two and a half orders in one write, then the missing half. The gateway
    // has to carry the half over to the next read without losing its place.
    std::array<wire::NewOrder, 3> orders{};
    for (std::size_t i = 0; i < orders.size(); ++i) {
        orders[i] = limit(3, core::Side::Buy, 950, 1, 40 + i);
    }
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(orders.data()),
                                           sizeof(orders)};
    const std::size_t cut = (2 * sizeof(wire::NewOrder)) + 13;
    alice->send_bytes(bytes.first(cut));
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 40U);
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 41U);
    EXPECT_TRUE(alice->quiet()) << "answered an order that has not fully arrived";

    alice->send_bytes(bytes.subspan(cut));
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 42U);
    EXPECT_EQ(gateway_->active_sessions(), 1U);
}

TEST_F(GatewayTest, ManyMessagesInOneWriteAreAllHandled) {
    const auto alice = client();
    alice->login(1);

    std::array<wire::NewOrder, 50> batch{};
    for (std::size_t i = 0; i < batch.size(); ++i) {
        batch[i] = limit(3, core::Side::Buy, 950, 1, 100 + i);
    }
    alice->send_bytes({reinterpret_cast<const std::byte*>(batch.data()), sizeof(batch)});
    for (std::size_t i = 0; i < batch.size(); ++i) {
        ASSERT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 100 + i);
    }
}

TEST_F(GatewayTest, BytesThatAreNotTheProtocolEndTheSession) {
    const auto vandal = client();
    vandal->login(1);
    const std::array<std::byte, 12> junk{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                         std::byte{0xEF}, std::byte{1},    std::byte{2},
                                         std::byte{3},    std::byte{4},    std::byte{5},
                                         std::byte{6},    std::byte{7},    std::byte{8}};
    vandal->send_bytes(junk);
    EXPECT_TRUE(vandal->closed_by_server());
    EXPECT_EQ(gateway_->stats().protocol_errors, 1U);
    EXPECT_EQ(gateway_->active_sessions(), 0U);
}

TEST_F(GatewayTest, ServerOnlyMessagesFromAClientEndTheSession) {
    const auto impostor = client();
    impostor->login(1);
    impostor->send(wire::OrderAccepted{});
    EXPECT_TRUE(impostor->closed_by_server());
    EXPECT_EQ(gateway_->stats().protocol_errors, 1U);
}

TEST_F(GatewayTest, LoginRules) {
    const auto first = client();
    first->login(42);

    const auto same_id = client();
    wire::LoginRequest request{};
    request.client_id = 42;
    same_id->send(request);
    EXPECT_EQ(same_id->expect<wire::LoginRejected>().reason, wire::LoginRejectReason::AlreadyLoggedIn);

    const auto old_version = client();
    wire::LoginRequest stale{};
    stale.client_id = 43;
    stale.protocol_version = 99;
    old_version->send(stale);
    EXPECT_EQ(old_version->expect<wire::LoginRejected>().reason,
              wire::LoginRejectReason::UnsupportedVersion);

    // Logging in twice on one connection is a protocol violation.
    first->send(request);
    EXPECT_TRUE(first->closed_by_server());

    // The id is free again once its holder has gone.
    const auto later = client();
    EXPECT_NE(later->login(42), 0);
}

TEST_F(GatewayTest, HeartbeatIsEchoedAndLogoutCloses) {
    const auto alice = client();
    alice->login(1);
    alice->send(wire::Heartbeat{});
    static_cast<void>(alice->expect<wire::Heartbeat>());

    alice->send(wire::Logout{});
    EXPECT_TRUE(alice->closed_by_server());
    EXPECT_EQ(gateway_->active_sessions(), 0U);
    EXPECT_EQ(gateway_->stats().protocol_errors, 0U) << "a logout is not an error";
}

TEST_F(GatewayTest, FullCommandRingIsReportedAsBusy) {
    start({}, 4);  // shard 2 takes four commands and then is full
    shards_running_ = false;

    const auto alice = client();
    alice->login(1);
    for (std::uint64_t i = 0; i < 6; ++i) {
        alice->send(limit(3, core::Side::Buy, 950, 1, i));
    }
    // Nothing is draining the shard, so the fifth and sixth cannot be queued.
    for (std::uint64_t i = 4; i < 6; ++i) {
        const auto rejected = alice->expect<wire::OrderRejected>();
        EXPECT_EQ(rejected.reason, wire::RejectCode::Busy);
        EXPECT_EQ(rejected.client_order_id, i);
    }
    EXPECT_EQ(gateway_->stats().busy, 2U);

    // Once the shard runs, the four that were queued are answered.
    shards_running_ = true;
    for (std::uint64_t i = 0; i < 4; ++i) {
        EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, i);
    }
}

TEST_F(GatewayTest, SessionLimitIsEnforced) {
    GatewayConfig config;
    config.max_sessions = 2;
    start(config);

    const auto first = client();
    const auto second = client();
    first->login(1);
    second->login(2);
    const auto third = client();
    EXPECT_TRUE(third->closed_by_server());
    EXPECT_EQ(gateway_->active_sessions(), 2U);

    // A slot freed by a departure can be taken by a newcomer.
    first->close();
    for (int i = 0; i < 20000 && gateway_->active_sessions() == 2; ++i) {
        pump();
        ::usleep(100);
    }
    const auto fourth = client();
    EXPECT_NE(fourth->login(4), 0);
}

TEST_F(GatewayTest, ReconnectingGivesANewIdentity) {
    auto ghost = client();
    const std::uint16_t ghost_session = ghost->login(1);
    ghost->send(limit(3, core::Side::Sell, 1000, 10, 1));
    const auto left_behind = ghost->expect<wire::OrderAccepted>();
    ghost->close();
    for (int i = 0; i < 20000 && gateway_->active_sessions() != 0; ++i) {
        pump();
        ::usleep(100);
    }
    ASSERT_EQ(gateway_->active_sessions(), 0U);

    // Same client id, new connection. It must not inherit the old session.
    const auto returned = client();
    const std::uint16_t new_session = returned->login(1);
    EXPECT_NE(new_session, ghost_session);

    wire::CancelOrder cancel{};
    cancel.order_id = left_behind.order_id;
    returned->send(cancel);
    EXPECT_EQ(returned->expect<wire::CancelRejected>().reason, wire::RejectCode::UnknownOrder)
        << "a new session cancelled an order from a dead one";

    // The abandoned order is still in the book and still trades.
    const auto buyer = client();
    buyer->login(2);
    buyer->send(limit(3, core::Side::Buy, 1000, 10, 2));
    static_cast<void>(buyer->expect<wire::OrderAccepted>());
    EXPECT_EQ(buyer->expect<wire::Executed>().qty, 10U);

    // Its fill report has nowhere to go and must not reach the newcomer.
    EXPECT_TRUE(returned->quiet());
    EXPECT_EQ(gateway_->stats().reports_dropped, 1U);
}

}  // namespace
}  // namespace planck::gateway
