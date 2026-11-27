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
        first.cancel_on_disconnect = cancel_on_disconnect_;
        engine::ShardConfig second = shard_config(5, kSpreads);
        second.cancel_on_disconnect = cancel_on_disconnect_;
        ASSERT_EQ(shards_[0]->init(first), Status::Ok);
        ASSERT_EQ(shards_[1]->init(second), Status::Ok);
        pointers_ = {shards_[0].get(), shards_[1].get()};

        config.listen = {net::kLoopback, 0};
        config.shards = pointers_;
        config.routes = kRoutes;
        config.idle = engine::IdlePolicy::Yield;
        gateway_ = std::make_unique<Gateway>();
        ASSERT_EQ(gateway_->init(config), Status::Ok);
        ASSERT_EQ(gateway_->local_endpoint(server_), Status::Ok);
        // A gateway opens by telling every shard that nobody is connected.
        // Let the shards take that in, so each test starts with empty rings.
        for (auto& shard : shards_) {
            ASSERT_EQ(shard->poll(), 1U);
            ASSERT_EQ(shard->stats().session_closes, 1U);
        }
    }

    // Drive everything until the gateway has this many clients left.
    void wait_until_nobody_is_connected_but(std::uint32_t remaining) {
        for (int i = 0; i < 20'000 && gateway_->active_sessions() != remaining; ++i) {
            pump();
            ::usleep(100);
        }
        ASSERT_EQ(gateway_->active_sessions(), remaining);
        pump();
    }
    void wait_until_nobody_is_connected() { wait_until_nobody_is_connected_but(0); }

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
    // Most tests are about the gateway alone and leave orders where they
    // lie. The ones about disconnects turn this on and start again.
    bool cancel_on_disconnect_ = false;
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
    const auto timers = [&](std::uint32_t heartbeat, std::uint32_t timeout) {
        return init_with([=](GatewayConfig& c) {
            c.heartbeat_interval_ms = heartbeat;
            c.idle_timeout_ms = timeout;
        });
    };
    EXPECT_EQ(timers(100, 100), Status::InvalidArgument) << "no time left to answer in";
    EXPECT_EQ(timers(100, 50), Status::InvalidArgument);
    EXPECT_EQ(timers(0, 500), Status::InvalidArgument)
        << "a venue that drops silent clients may not be silent itself";
    EXPECT_EQ(timers(100, 65'536), Status::OutOfRange) << "the client is told it in 16 bits";
    EXPECT_EQ(timers(65'536, 0), Status::OutOfRange);
    const auto holding = [&](std::uint32_t reports, std::uint32_t sessions) {
        return init_with([=](GatewayConfig& c) {
            c.retain_reports = reports;
            c.retain_sessions = sessions;
        });
    };
    EXPECT_EQ(holding(6, 4), Status::InvalidArgument) << "reports are held in rings";
    EXPECT_EQ(holding(8, 0), Status::InvalidArgument) << "room for reports and nobody to have it";
    EXPECT_EQ(holding(0, 4), Status::InvalidArgument);
    EXPECT_EQ(holding(8, 65'536), Status::OutOfRange);
    EXPECT_EQ(holding(1U << 20, 32), Status::OutOfRange) << "more than a gateway may hold";
    static constexpr std::uint64_t kTooFew[100] = {};
    EXPECT_EQ(init_with([](GatewayConfig& c) { c.last_order_ids = kTooFew; }),
              Status::InvalidArgument)
        << "an order id for some sessions and not others";
    EXPECT_EQ(init_with([&](GatewayConfig& c) { c.shards = twice; }), Status::Duplicate);
    EXPECT_EQ(init_with([&](GatewayConfig& c) { c.shards = cold; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](GatewayConfig& c) { c.routes = kWrongShard; }), Status::OutOfRange);
    EXPECT_EQ(init_with([](GatewayConfig& c) { c.routes = kNotTraded; }), Status::NotFound)
        << "a route to a shard that does not trade the symbol";
}

TEST_F(GatewayTest, LoginThenOrderIsAcknowledged) {
    const auto alice = client();
    EXPECT_EQ(alice->login(1001), 1001) << "a session id is the client id";
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

// Send a login that must be refused, and check that the gateway says why
// and then hangs up.
void expect_refused(WireClient& client, std::uint32_t client_id, std::uint64_t token,
                    wire::LoginRejectReason reason) {
    wire::LoginRequest request{};
    request.client_id = client_id;
    request.token = token;
    client.send(request);
    EXPECT_EQ(client.expect<wire::LoginRejected>().reason, reason) << "client id " << client_id;
    EXPECT_TRUE(client.closed_by_server()) << "a refused login must end the connection";
}

TEST_F(GatewayTest, LoginRules) {
    const auto first = client();
    first->login(42);

    const auto same_id = client();
    expect_refused(*same_id, 42, 0, wire::LoginRejectReason::AlreadyLoggedIn);

    const auto old_version = client();
    wire::LoginRequest stale{};
    stale.client_id = 43;
    stale.protocol_version = 99;
    old_version->send(stale);
    EXPECT_EQ(old_version->expect<wire::LoginRejected>().reason,
              wire::LoginRejectReason::UnsupportedVersion);
    EXPECT_TRUE(old_version->closed_by_server());

    // A client id is a session id, so it must fit in one and may not be zero.
    const auto nobody = client();
    expect_refused(*nobody, 0, 0, wire::LoginRejectReason::BadCredentials);
    const auto too_big = client();
    expect_refused(*too_big, 65'536, 0, wire::LoginRejectReason::BadCredentials);
    const auto wraps_to_42 = client();
    expect_refused(*wraps_to_42, 65'536 + 42, 0, wire::LoginRejectReason::BadCredentials);
    const auto largest = client();
    EXPECT_EQ(largest->login(65'535), 65'535);

    // Logging in twice on one connection is a protocol violation.
    wire::LoginRequest again{};
    again.client_id = 42;
    first->send(again);
    EXPECT_TRUE(first->closed_by_server());

    // The id is free again once its holder has gone.
    const auto later = client();
    EXPECT_EQ(later->login(42), 42);
    EXPECT_EQ(gateway_->stats().bad_credentials, 3U);
}

TEST_F(GatewayTest, WithAClientListOnlyTheRightTokenGetsIn) {
    const auto credentials = std::make_unique<Credentials>();
    ASSERT_EQ(credentials->add(7, 0xfeedULL), Status::Ok);
    ASSERT_EQ(credentials->add(8, 0), Status::Ok);
    GatewayConfig config;
    config.credentials = credentials.get();
    start(config);

    const auto wrong_token = client();
    expect_refused(*wrong_token, 7, 0xfeeeULL, wire::LoginRejectReason::BadCredentials);
    const auto no_token = client();
    expect_refused(*no_token, 7, 0, wire::LoginRejectReason::BadCredentials);
    const auto someone_elses = client();
    expect_refused(*someone_elses, 8, 0xfeedULL, wire::LoginRejectReason::BadCredentials);
    const auto not_listed = client();
    expect_refused(*not_listed, 9, 0xfeedULL, wire::LoginRejectReason::BadCredentials);
    EXPECT_EQ(gateway_->stats().logins, 0U);
    EXPECT_EQ(gateway_->stats().bad_credentials, 4U);

    // A refused client reached no shard, before or after the refusal.
    const auto pushy = client();
    wire::LoginRequest bad{};
    bad.client_id = 7;
    pushy->send(bad);
    pushy->send(limit(3, core::Side::Buy, 1000, 10, 1));
    static_cast<void>(pushy->expect<wire::LoginRejected>());
    EXPECT_TRUE(pushy->closed_by_server());
    EXPECT_EQ(gateway_->stats().commands_out, 0U);

    const auto seven = client();
    EXPECT_EQ(seven->login(7, 0xfeedULL), 7);
    const auto eight = client();
    EXPECT_EQ(eight->login(8, 0), 8) << "a token of zero is a token like any other";
    seven->send(limit(3, core::Side::Buy, 1000, 10, 2));
    EXPECT_EQ(seven->expect<wire::OrderAccepted>().client_order_id, 2U);
}

TEST_F(GatewayTest, AHeartbeatIsNotAnsweredAndLogoutCloses) {
    const auto alice = client();
    alice->login(1);
    alice->send(wire::Heartbeat{});
    alice->send(wire::Heartbeat{});
    EXPECT_TRUE(alice->quiet()) << "two ends that both answered heartbeats would never stop";
    EXPECT_EQ(gateway_->stats().heartbeats_in, 2U);
    EXPECT_EQ(gateway_->active_sessions(), 1U);

    alice->send(wire::Logout{});
    EXPECT_TRUE(alice->closed_by_server());
    EXPECT_EQ(gateway_->active_sessions(), 0U);
    EXPECT_EQ(gateway_->stats().protocol_errors, 0U) << "a logout is not an error";
}

// The time the gateway is told it is, for the tests about silence. Real
// time would make them slow, and wrong on a busy machine.
std::uint64_t g_now_ns = 1'000'000'000;
std::uint64_t test_clock() noexcept { return g_now_ns; }

class GatewayTimers : public GatewayTest {
protected:
    static constexpr std::uint32_t kHeartbeatMs = 100;
    static constexpr std::uint32_t kTimeoutMs = 500;

    void SetUp() override {
        cancel_on_disconnect_ = true;
        GatewayConfig config;
        config.heartbeat_interval_ms = kHeartbeatMs;
        config.idle_timeout_ms = kTimeoutMs;
        config.clock = test_clock;
        start(config);
    }

    // Let time pass a little at a time with the gateway running, as it
    // would. A jump of 100 ms or more would look to the gateway like a
    // pause of its own.
    void pass(std::uint32_t ms) {
        for (std::uint32_t done = 0; done < ms; done += 10) {
            g_now_ns += 10'000'000;
            pump();
        }
    }

    // Read every heartbeat waiting for this client and count them.
    static std::size_t heartbeats(WireClient& client) {
        std::size_t count = 0;
        while (client.pending<wire::Heartbeat>()) {
            static_cast<void>(client.expect<wire::Heartbeat>());
            ++count;
        }
        return count;
    }
};

TEST_F(GatewayTimers, TheClientIsToldHowLongItMayBeSilent) {
    const auto alice = client();
    wire::LoginRequest request{};
    request.client_id = 1;
    alice->send(request);
    EXPECT_EQ(alice->expect<wire::LoginAccepted>().idle_timeout_ms, kTimeoutMs);

    start({});  // and a gateway without a timeout says so
    const auto bob = client();
    bob->send(request);
    EXPECT_EQ(bob->expect<wire::LoginAccepted>().idle_timeout_ms, 0);
}

TEST_F(GatewayTimers, ASilentClientIsDroppedAndItsOrdersCancelled) {
    const auto silent = client();
    silent->login(1);
    silent->send(limit(3, core::Side::Buy, 950, 1, 1));
    static_cast<void>(silent->expect<wire::OrderAccepted>());
    const auto lively = client();
    lively->login(2);

    // Just short of the limit it is still there. The other client keeps
    // talking throughout.
    for (std::uint32_t waited = 0; waited < kTimeoutMs - 50; waited += 50) {
        pass(50);
        lively->send(wire::Heartbeat{});
    }
    pump();
    EXPECT_EQ(gateway_->active_sessions(), 2U) << "dropped before its time was up";
    EXPECT_EQ(gateway_->stats().idle_timeouts, 0U);

    for (int i = 0; i < 4; ++i) {
        pass(50);
        lively->send(wire::Heartbeat{});
    }
    pump();
    EXPECT_EQ(gateway_->stats().idle_timeouts, 1U);
    EXPECT_EQ(gateway_->active_sessions(), 1U) << "the client that kept talking must stay";
    EXPECT_TRUE(silent->closed_by_server());
    EXPECT_EQ(shards_[0]->stats().disconnect_cancels, 1U)
        << "a dead line is a disconnect like any other";
    EXPECT_EQ(shards_[0]->store().in_use(), 0U);
}

TEST_F(GatewayTimers, AnythingAClientSendsCountsAsBeingHeard) {
    const auto alice = client();
    alice->login(1);
    for (int i = 0; i < 12; ++i) {  // well over two timeouts in all
        pass(kTimeoutMs / 5);
        if (i % 2 == 0) {
            alice->send(limit(3, core::Side::Buy, 5000, 1, 1));  // rejected, but heard
        } else {
            alice->send(wire::Heartbeat{});
        }
        pump();
    }
    EXPECT_EQ(gateway_->stats().idle_timeouts, 0U);
    EXPECT_EQ(gateway_->active_sessions(), 1U);
}

TEST_F(GatewayTimers, AQuietClientIsSentHeartbeats) {
    const auto alice = client();
    alice->login(1);
    EXPECT_EQ(heartbeats(*alice), 0U) << "a heartbeat right behind the login answer";

    // It keeps itself alive with one message, and is sent nothing else.
    pass(kHeartbeatMs / 2);
    EXPECT_EQ(heartbeats(*alice), 0U) << "a heartbeat before the venue had been quiet long enough";
    pass(kHeartbeatMs);
    EXPECT_EQ(heartbeats(*alice), 1U);
    alice->send(wire::Heartbeat{});
    pass(3 * kHeartbeatMs);
    const std::size_t more = heartbeats(*alice);
    EXPECT_GE(more, 2U);
    EXPECT_LE(more, 3U) << "about one for each interval, and no more";
    EXPECT_EQ(gateway_->stats().heartbeats_out, 1 + more);
}

TEST_F(GatewayTimers, AClientThatIsBeingToldThingsIsNotSentHeartbeatsToo) {
    const auto alice = client();
    alice->login(1);
    for (int i = 0; i < 10; ++i) {
        pass(kHeartbeatMs / 2);
        alice->send(limit(3, core::Side::Buy, 5000, 1, static_cast<std::uint64_t>(i) + 1));
        EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::PriceOutOfBand);
    }
    EXPECT_EQ(gateway_->stats().heartbeats_out, 0U);
    EXPECT_TRUE(alice->quiet());
}

TEST_F(GatewayTimers, AConnectionMustLogInInTimeWhateverElseItSays) {
    const auto chatty = client();
    const auto mute = client();
    for (std::uint32_t waited = 0; waited < kTimeoutMs - 50; waited += 50) {
        pass(50);
        chatty->send(wire::Heartbeat{});
    }
    pump();
    EXPECT_EQ(gateway_->active_sessions(), 2U);
    pass(150);
    EXPECT_TRUE(chatty->closed_by_server()) << "talking is not logging in";
    EXPECT_TRUE(mute->closed_by_server());
    EXPECT_EQ(gateway_->stats().login_timeouts, 1U);
    EXPECT_EQ(gateway_->stats().idle_timeouts, 1U);
    EXPECT_EQ(gateway_->stats().heartbeats_out, 0U) << "heartbeats are for clients, not strangers";
    EXPECT_EQ(gateway_->stats().session_closes, 0U) << "neither was ever a session";
}

TEST_F(GatewayTimers, AGatewayThatWasHeldUpDoesNotBlameItsClients) {
    const auto alice = client();
    alice->login(1);
    alice->send(limit(3, core::Side::Buy, 950, 1, 1));
    static_cast<void>(alice->expect<wire::OrderAccepted>());
    const auto stranger = client();
    pump();
    ASSERT_EQ(gateway_->active_sessions(), 2U);

    // Ten timeouts go by in one step: the gateway was not running. Whatever
    // its clients sent meanwhile it has not read yet.
    g_now_ns += std::uint64_t{10} * kTimeoutMs * 1'000'000;
    for (int i = 0; i < 20; ++i) {
        pump();
    }
    EXPECT_EQ(gateway_->stats().idle_timeouts, 0U);
    EXPECT_EQ(gateway_->stats().login_timeouts, 0U);
    EXPECT_EQ(gateway_->active_sessions(), 2U);
    EXPECT_EQ(shards_[0]->store().in_use(), 1U);

    // From there the usual rule applies again, counted from the pause.
    pass(kTimeoutMs - 100);
    EXPECT_EQ(gateway_->active_sessions(), 2U);
    pass(200);
    EXPECT_EQ(gateway_->stats().idle_timeouts, 2U);
    EXPECT_EQ(gateway_->active_sessions(), 0U);
    EXPECT_EQ(shards_[0]->store().in_use(), 0U);
}

TEST_F(GatewayTest, WithoutTimersSilenceIsAllowedForEver) {
    GatewayConfig config;
    config.clock = test_clock;
    start(config);
    const auto alice = client();
    alice->login(1);
    for (int i = 0; i < 50; ++i) {
        g_now_ns += 50'000'000;
        pump();
    }
    g_now_ns += 3'600'000'000'000;
    pump();
    EXPECT_EQ(gateway_->active_sessions(), 1U);
    EXPECT_TRUE(alice->quiet());
    EXPECT_EQ(gateway_->stats().idle_timeouts, 0U);
    EXPECT_EQ(gateway_->stats().heartbeats_out, 0U);
}

TEST_F(GatewayTest, FullCommandRingIsReportedAsBusy) {
    start({}, 4);  // shard 2 takes four commands and then is full
    shards_running_ = false;

    const auto alice = client();
    alice->login(1);
    for (std::uint64_t i = 1; i <= 6; ++i) {
        alice->send(limit(3, core::Side::Buy, 950, 1, i));
    }
    // Nothing is draining the shard, so the fifth and sixth cannot be queued.
    for (std::uint64_t i = 5; i <= 6; ++i) {
        const auto rejected = alice->expect<wire::OrderRejected>();
        EXPECT_EQ(rejected.reason, wire::RejectCode::Busy);
        EXPECT_EQ(rejected.client_order_id, i);
    }
    EXPECT_EQ(gateway_->stats().busy, 2U);

    // Once the shard runs, the four that were queued are answered.
    shards_running_ = true;
    for (std::uint64_t i = 1; i <= 4; ++i) {
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
    for (int i = 0; i < 20'000 && gateway_->active_sessions() == 2; ++i) {
        pump();
        ::usleep(100);
    }
    const auto fourth = client();
    EXPECT_NE(fourth->login(4), 0);
}

TEST_F(GatewayTest, ADroppedConnectionCancelsItsOrdersInEveryShard) {
    cancel_on_disconnect_ = true;
    start({});
    auto maker = client();
    maker->login(1);
    maker->send(limit(3, core::Side::Sell, 1000, 10, 1));
    static_cast<void>(maker->expect<wire::OrderAccepted>());
    maker->send(limit(700, core::Side::Sell, -50, 3, 2));
    static_cast<void>(maker->expect<wire::OrderAccepted>());
    const auto bystander = client();
    bystander->login(2);
    bystander->send(limit(3, core::Side::Sell, 1001, 4, 3));
    static_cast<void>(bystander->expect<wire::OrderAccepted>());

    maker->close();
    wait_until_nobody_is_connected_but(1);
    EXPECT_EQ(gateway_->stats().session_closes, 2U) << "one for each shard";
    EXPECT_EQ(gateway_->stats().session_closes_delayed, 0U);
    EXPECT_EQ(shards_[0]->stats().disconnect_cancels, 1U);
    EXPECT_EQ(shards_[1]->stats().disconnect_cancels, 1U);
    EXPECT_EQ(shards_[0]->store().in_use(), 1U) << "the bystander's order must stay";
    EXPECT_EQ(shards_[1]->store().in_use(), 0U);
    // The two cancellations were for a client that was no longer there.
    EXPECT_EQ(gateway_->stats().reports_unsent, 2U);
    EXPECT_TRUE(bystander->quiet()) << "one client was told about another's disconnect";

    // Nothing is left to trade with at the maker's price.
    bystander->send(limit(3, core::Side::Buy, 1000, 10, 4));
    static_cast<void>(bystander->expect<wire::OrderAccepted>());
    EXPECT_TRUE(bystander->quiet()) << "traded with an order that should have been cancelled";

    // The maker comes back to the same session and an empty slate.
    const auto returned = client();
    EXPECT_EQ(returned->login(1), 1);
    EXPECT_TRUE(returned->quiet());
    returned->send(limit(3, core::Side::Sell, 1005, 1, 5));
    EXPECT_EQ(returned->expect<wire::OrderAccepted>().client_order_id, 5U);
}

TEST_F(GatewayTest, AClientBackBeforeTheShardHasActedHearsItsOrdersCancelled) {
    cancel_on_disconnect_ = true;
    start({});
    auto first = client();
    first->login(1);
    first->send(limit(3, core::Side::Sell, 1000, 10, 1));
    const auto resting = first->expect<wire::OrderAccepted>();

    // The shard falls behind. The connection ends and the close is queued
    // for it, so nothing is owed and the client may come straight back.
    shards_running_ = false;
    first->close();
    wait_until_nobody_is_connected();
    ASSERT_EQ(gateway_->stats().session_closes_delayed, 0U);
    const auto second = client();
    ASSERT_EQ(second->login(1), 1);
    second->send(limit(3, core::Side::Sell, 1001, 2, 2));

    // The shard catches up: first the close, which takes the old order and
    // tells the session, now connected again. Then the new order, which
    // came after the close and must stay.
    shards_running_ = true;
    const auto canceled = second->expect<wire::Canceled>();
    EXPECT_EQ(canceled.order_id, resting.order_id);
    EXPECT_EQ(canceled.qty, 10U);
    EXPECT_EQ(canceled.reason, wire::CancelReason::Disconnected);
    EXPECT_EQ(second->expect<wire::OrderAccepted>().client_order_id, 2U);
    EXPECT_TRUE(second->quiet());
    EXPECT_EQ(shards_[0]->store().in_use(), 1U);
    EXPECT_EQ(gateway_->stats().reports_unsent, 0U);
}

TEST_F(GatewayTest, EveryWayOfLeavingCancels) {
    cancel_on_disconnect_ = true;
    start({});
    const auto resting_order = [](WireClient& who, std::uint32_t id) {
        who.login(id);
        who.send(limit(3, core::Side::Buy, 950, 1, id));
        static_cast<void>(who.expect<wire::OrderAccepted>());
    };

    const auto polite = client();
    resting_order(*polite, 1);
    polite->send(wire::Logout{});
    EXPECT_TRUE(polite->closed_by_server());

    const auto vandal = client();
    resting_order(*vandal, 2);
    vandal->send(wire::OrderAccepted{});  // not a message a client may send
    EXPECT_TRUE(vandal->closed_by_server());

    auto vanished = client();
    resting_order(*vanished, 3);
    vanished->close();

    wait_until_nobody_is_connected();
    EXPECT_EQ(shards_[0]->stats().disconnect_cancels, 3U);
    EXPECT_EQ(shards_[0]->store().in_use(), 0U);
    EXPECT_EQ(gateway_->stats().session_closes, 6U);

    // A connection that never logged in is no session and closes nothing.
    auto nobody = client();
    nobody->send(limit(3, core::Side::Buy, 950, 1, 1));
    static_cast<void>(nobody->expect<wire::OrderRejected>());
    nobody->close();
    wait_until_nobody_is_connected();
    const auto refused = client();
    wire::LoginRequest bad{};
    bad.client_id = 0;
    refused->send(bad);
    static_cast<void>(refused->expect<wire::LoginRejected>());
    EXPECT_TRUE(refused->closed_by_server());
    EXPECT_EQ(gateway_->stats().session_closes, 6U);
}

TEST_F(GatewayTest, ACloseThatDoesNotFitIsOwedAndTheSessionWaitsForIt) {
    cancel_on_disconnect_ = true;
    start({}, 4);  // shard 2 takes four commands and then is full
    shards_running_ = false;

    auto leaver = client();
    leaver->login(1);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        leaver->send(limit(3, core::Side::Buy, 950, 1, i));
    }
    for (int i = 0; i < 50; ++i) {
        pump();
    }
    ASSERT_EQ(gateway_->stats().commands_out, 4U);
    leaver->close();
    wait_until_nobody_is_connected();

    // Shard 5 heard at once. Shard 2 has no room, so the gateway owes it.
    EXPECT_EQ(gateway_->stats().session_closes, 2U);
    EXPECT_EQ(gateway_->stats().session_closes_delayed, 1U);

    // Until that is settled the session cannot come back: its four orders
    // are still to rest, and the close that follows them would otherwise
    // cancel whatever its new connection did in between.
    const auto too_soon = client();
    expect_refused(*too_soon, 1, 0, wire::LoginRejectReason::AlreadyLoggedIn);
    const auto someone_else = client();
    EXPECT_EQ(someone_else->login(2), 2) << "one session's backlog held up another";

    // The shard runs, the four orders rest, the close arrives and takes them.
    shards_running_ = true;
    for (int i = 0; i < 50; ++i) {
        pump();
    }
    EXPECT_EQ(shards_[0]->stats().accepted, 4U);
    EXPECT_EQ(shards_[0]->stats().disconnect_cancels, 4U);
    EXPECT_EQ(shards_[0]->store().in_use(), 0U);
    EXPECT_EQ(shards_[0]->stats().session_closes, 2U) << "the close was sent more than once";

    const auto back = client();
    EXPECT_EQ(back->login(1), 1);
    back->send(limit(3, core::Side::Buy, 950, 1, 9));
    static_cast<void>(back->expect<wire::OrderAccepted>());
    for (int i = 0; i < 50; ++i) {
        pump();
    }
    EXPECT_EQ(shards_[0]->store().in_use(), 1U) << "a late close cancelled the new connection's order";
}

TEST_F(GatewayTest, AStartingGatewayClearsWhatAnEarlierOneLeft) {
    cancel_on_disconnect_ = true;
    start({});
    const auto early = client();
    early->login(1);
    early->send(limit(3, core::Side::Buy, 950, 1, 1));
    static_cast<void>(early->expect<wire::OrderAccepted>());
    ASSERT_EQ(shards_[0]->store().in_use(), 1U);

    // The gateway goes away without a word, as in a crash, and another
    // takes its place in front of the same shards.
    GatewayConfig config;
    config.listen = {net::kLoopback, 0};
    config.shards = pointers_;
    config.routes = kRoutes;
    gateway_ = std::make_unique<Gateway>();
    ASSERT_EQ(gateway_->init(config), Status::Ok);
    for (auto& shard : shards_) {
        static_cast<void>(shard->poll());
    }
    EXPECT_EQ(shards_[0]->store().in_use(), 0U);
    EXPECT_EQ(shards_[0]->stats().disconnect_cancels, 1U);
    EXPECT_EQ(shards_[0]->stats().session_closes, 2U);
}

TEST_F(GatewayTest, AReconnectingClientIsTheSameOwner) {
    // With shards that keep orders, which is not how a venue runs but shows
    // that a session is its client id and nothing else.
    auto maker = client();
    ASSERT_EQ(maker->login(1), 1);
    maker->send(limit(3, core::Side::Sell, 1000, 10, 1));
    const auto resting = maker->expect<wire::OrderAccepted>();
    maker->close();
    for (int i = 0; i < 20'000 && gateway_->active_sessions() != 0; ++i) {
        pump();
        ::usleep(100);
    }
    ASSERT_EQ(gateway_->active_sessions(), 0U);

    // The order goes on working while its owner is away. The owner's report
    // of this fill has nobody to go to, and that is counted.
    const auto buyer = client();
    buyer->login(2);
    buyer->send(limit(3, core::Side::Buy, 1000, 4, 2));
    static_cast<void>(buyer->expect<wire::OrderAccepted>());
    EXPECT_EQ(buyer->expect<wire::Executed>().qty, 4U);
    EXPECT_EQ(gateway_->stats().reports_unsent, 1U);

    // Nobody else can touch it meanwhile.
    wire::CancelOrder cancel{};
    cancel.order_id = resting.order_id;
    buyer->send(cancel);
    EXPECT_EQ(buyer->expect<wire::CancelRejected>().reason, wire::RejectCode::UnknownOrder)
        << "one client cancelled another's order";

    // Same client id, new connection: the same session, and the same owner.
    const auto returned = client();
    EXPECT_EQ(returned->login(1), 1);
    EXPECT_TRUE(returned->quiet()) << "it asked for nothing from before, and must get nothing";

    buyer->send(limit(3, core::Side::Buy, 1000, 1, 3));
    static_cast<void>(buyer->expect<wire::OrderAccepted>());
    static_cast<void>(buyer->expect<wire::Executed>());
    const auto fill = returned->expect<wire::Executed>();
    EXPECT_EQ(fill.order_id, resting.order_id);
    EXPECT_EQ(fill.liquidity, wire::Liquidity::Maker);
    EXPECT_EQ(fill.leaves, 5U);

    returned->send(cancel);
    const auto canceled = returned->expect<wire::Canceled>();
    EXPECT_EQ(canceled.order_id, resting.order_id);
    EXPECT_EQ(canceled.qty, 5U);
    EXPECT_EQ(gateway_->stats().reports_unsent, 1U);
}

TEST_F(GatewayTest, ConnectionsCanComeAndGoWithoutLimit) {
    // More connections than there are slots, and one session throughout.
    // Every slot gets used again, and the session never wears out.
    GatewayConfig config;
    config.max_sessions = 2;
    start(config);
    const auto resident = client();
    ASSERT_EQ(resident->login(9), 9);

    const int rounds = static_cast<int>(core::kMaxSessions) + 100;
    for (int round = 0; round < rounds; ++round) {
        const auto visitor = client();
        ASSERT_EQ(visitor->login(5), 5) << "round " << round;
        visitor->send(wire::Logout{});
        ASSERT_TRUE(visitor->closed_by_server()) << "round " << round;
    }
    EXPECT_EQ(gateway_->active_sessions(), 1U);
    EXPECT_EQ(gateway_->stats().logins, static_cast<std::uint64_t>(rounds) + 1);

    resident->send(limit(3, core::Side::Buy, 1000, 10, 1));
    EXPECT_EQ(resident->expect<wire::OrderAccepted>().client_order_id, 1U)
        << "the connection that stayed was disturbed by the ones that came and went";
}

// ---- numbered reports, recovery, and orders carried out at most once ----

// The same two shards, with a gateway that holds each session's last 64
// reports, and room in a connection for only two at a time: so that sending
// held reports takes many passes, as it would to a slow or distant client.
class GatewayRecovery : public GatewayTest {
protected:
    static constexpr std::uint64_t kEpoch = 77;
    static constexpr std::uint32_t kHeld = 64;

    static GatewayConfig holding(std::uint32_t reports = kHeld, std::uint32_t sessions = 4) {
        GatewayConfig config;
        config.retain_reports = reports;
        config.retain_sessions = sessions;
        config.epoch = kEpoch;
        config.tx_buffer_bytes = 2 * wire::kMaxMessageSize;
        return config;
    }

    void SetUp() override { start(holding()); }

    // Client 1 offers plenty at 1000 and loses its connection. Client 2 then
    // buys one lot at a time, `fills` times. So client 1 was sent report 1,
    // its OrderAccepted, and has missed reports 2 to fills + 1, all fills.
    wire::OrderAccepted fills_while_away(std::uint32_t fills) {
        auto maker = client();
        EXPECT_EQ(maker->resume(1, 0, 0).next_report, 1U);
        maker->send(limit(3, core::Side::Sell, 1000, 1000, 1));
        const auto resting = maker->expect<wire::OrderAccepted>();
        EXPECT_EQ(maker->next_report(), 2U);
        maker->close();
        wait_until_nobody_is_connected();

        const auto buyer = client();
        buyer->login(2);
        for (std::uint32_t i = 1; i <= fills; ++i) {
            buyer->send(limit(3, core::Side::Buy, 1000, 1, i));
            static_cast<void>(buyer->expect<wire::OrderAccepted>());
            static_cast<void>(buyer->expect<wire::Executed>());
        }
        buyer->close();
        wait_until_nobody_is_connected();
        return resting;
    }

    // The next `count` messages must be client 1's fills, in order: each
    // leaves one lot fewer than the one before.
    static void expect_fills(WireClient& maker, std::uint32_t first_leaves, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count && !::testing::Test::HasFailure(); ++i) {
            ASSERT_EQ(maker.expect<wire::Executed>().leaves, first_leaves - i) << "fill " << i;
        }
    }
};

TEST_F(GatewayRecovery, LoginSaysWhereTheNumbersStand) {
    EXPECT_EQ(gateway_->epoch(), kEpoch);
    auto alice = client();
    wire::LoginAccepted accepted = alice->resume(1, 0, 0);
    EXPECT_EQ(accepted.epoch, kEpoch);
    EXPECT_EQ(accepted.next_report, 1U) << "report numbers start at 1";
    EXPECT_EQ(accepted.live_from, 1U);
    EXPECT_EQ(accepted.last_client_order_id, 0U);

    alice->send(limit(3, core::Side::Buy, 950, 1, 10));
    static_cast<void>(alice->expect<wire::OrderAccepted>());
    alice->send(limit(9, core::Side::Buy, 950, 1, 20));
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::UnknownSymbol);
    EXPECT_EQ(alice->next_report(), 3U);
    alice->close();
    wait_until_nobody_is_connected();

    // A client that asks for nothing from before starts where things stand.
    alice = client();
    accepted = alice->resume(1, 0, 0);
    EXPECT_EQ(accepted.next_report, 3U);
    EXPECT_EQ(accepted.live_from, 3U);
    EXPECT_EQ(accepted.last_client_order_id, 20U);
    EXPECT_TRUE(alice->quiet());
    EXPECT_EQ(gateway_->stats().recoveries, 0U);
}

TEST_F(GatewayTest, AGatewayNamesItsOwnRunIfNobodyElseDoes) {
    EXPECT_NE(gateway_->epoch(), 0U) << "zero is what a client with no earlier connection sends";
    const auto alice = client();
    EXPECT_EQ(alice->resume(1, 0, 0).epoch, gateway_->epoch());
}

TEST_F(GatewayRecovery, WhatIsSaidBeforeLoginHasNoNumber) {
    const auto alice = client();
    alice->send(limit(3, core::Side::Buy, 950, 1, 1));
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::NotLoggedIn);
    const wire::LoginAccepted accepted = alice->resume(1, 0, 0);
    EXPECT_EQ(accepted.next_report, 1U);
    EXPECT_EQ(accepted.last_client_order_id, 0U) << "an order from nobody used somebody's id";
    alice->send(limit(3, core::Side::Buy, 950, 1, 1));
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 1U);
}

TEST_F(GatewayRecovery, AClientIsSentTheReportsItMissed) {
    const auto resting = fills_while_away(5);
    EXPECT_EQ(gateway_->stats().reports_unsent, 5U);

    // It had seen report 1 and asks for 2 onwards.
    const auto maker = client();
    const wire::LoginAccepted accepted = maker->resume(1, kEpoch, 2);
    EXPECT_EQ(accepted.next_report, 2U);
    EXPECT_EQ(accepted.live_from, 7U);
    expect_fills(*maker, 999, 5);
    EXPECT_TRUE(maker->quiet()) << "more came than was missed";
    EXPECT_EQ(maker->next_report(), 7U);
    EXPECT_EQ(gateway_->stats().recoveries, 1U);
    EXPECT_EQ(gateway_->stats().reports_recovered, 5U);
    EXPECT_EQ(gateway_->stats().recovery_gaps, 0U);

    // And then it is an ordinary connection again.
    wire::CancelOrder cancel{};
    cancel.order_id = resting.order_id;
    maker->send(cancel);
    EXPECT_EQ(maker->expect<wire::Canceled>().qty, 995U);
    EXPECT_EQ(maker->next_report(), 8U);
    EXPECT_EQ(gateway_->stats().reports_recovered, 5U);
}

TEST_F(GatewayRecovery, AClientMayAskAgainForWhatItHasSeen) {
    const auto resting = fills_while_away(2);
    const auto maker = client();
    const wire::LoginAccepted accepted = maker->resume(1, kEpoch, 1);
    EXPECT_EQ(accepted.next_report, 1U);
    EXPECT_EQ(accepted.live_from, 4U);
    EXPECT_EQ(maker->expect<wire::OrderAccepted>().order_id, resting.order_id);
    expect_fills(*maker, 999, 2);
    EXPECT_TRUE(maker->quiet());
}

TEST_F(GatewayRecovery, AClientThatHasMissedNothingIsSentNothing) {
    static_cast<void>(fills_while_away(2));
    const auto maker = client();
    const wire::LoginAccepted accepted = maker->resume(1, kEpoch, 4);
    EXPECT_EQ(accepted.next_report, 4U);
    EXPECT_EQ(accepted.live_from, 4U);
    EXPECT_TRUE(maker->quiet());
    EXPECT_EQ(gateway_->stats().recoveries, 0U);
}

TEST_F(GatewayRecovery, ReportsNoLongerHeldAreGoneAndTheClientCanTell) {
    // 70 fills and the OrderAccepted make 71 reports. The last 64 are held:
    // numbers 8 to 71.
    static_cast<void>(fills_while_away(70));
    const auto maker = client();
    const wire::LoginAccepted accepted = maker->resume(1, kEpoch, 2);
    EXPECT_EQ(accepted.next_report, 8U) << "reports 2 to 7 are gone, and this is how it is told";
    EXPECT_EQ(accepted.live_from, 72U);
    EXPECT_EQ(gateway_->stats().recovery_gaps, 1U);
    // Report 8 is the seventh fill, which left 993.
    expect_fills(*maker, 993, 64);
    EXPECT_TRUE(maker->quiet());
    EXPECT_EQ(gateway_->stats().reports_recovered, 64U);
}

TEST_F(GatewayRecovery, NumbersFromAnotherRunOfTheVenueMeanNothing) {
    const auto resting = fills_while_away(3);
    // It counted to 40 before the venue restarted. Here that says nothing
    // about what it has seen, so it is sent everything held.
    const auto maker = client();
    const wire::LoginAccepted accepted = maker->resume(1, kEpoch - 1, 40);
    EXPECT_EQ(accepted.epoch, kEpoch);
    EXPECT_EQ(accepted.next_report, 1U);
    EXPECT_EQ(accepted.live_from, 5U);
    EXPECT_EQ(maker->expect<wire::OrderAccepted>().order_id, resting.order_id);
    expect_fills(*maker, 999, 3);
    EXPECT_EQ(gateway_->stats().recovery_gaps, 0U) << "nobody knows what it missed";
}

TEST_F(GatewayRecovery, AClientAheadOfTheVenueIsRefused) {
    static_cast<void>(fills_while_away(2));
    // Three reports exist, so 4 is the most a client can be waiting for.
    const auto confused = client();
    wire::LoginRequest request{};
    request.client_id = 1;
    request.epoch = kEpoch;
    request.next_report = 5;
    confused->send(request);
    EXPECT_EQ(confused->expect<wire::LoginRejected>().reason,
              wire::LoginRejectReason::BadSequence);
    EXPECT_TRUE(confused->closed_by_server());

    const auto maker = client();
    EXPECT_EQ(maker->resume(1, kEpoch, 4).next_report, 4U) << "the refusal left a mark";
}

TEST_F(GatewayRecovery, NewReportsWaitTheirTurnBehindHeldOnes) {
    static_cast<void>(fills_while_away(60));
    const auto maker = client();
    ASSERT_EQ(maker->resume(1, kEpoch, 2).live_from, 62U);
    // Sixty reports at two a pass: the gateway is still sending them when
    // this arrives, and the answer to it must come after every one.
    maker->send(limit(9, core::Side::Buy, 950, 1, 2));
    expect_fills(*maker, 999, 60);
    EXPECT_EQ(maker->expect<wire::OrderRejected>().reason, wire::RejectCode::UnknownSymbol);
    EXPECT_EQ(maker->next_report(), 63U);
    EXPECT_EQ(gateway_->stats().reports_recovered, 61U)
        << "the answer did not arrive while the client was behind, so this test showed nothing";
    EXPECT_EQ(gateway_->stats().slow_consumers, 0U);

    // Once it has caught up, reports go straight out again.
    const std::uint64_t direct = gateway_->stats().reports_out;
    maker->send(limit(9, core::Side::Buy, 950, 1, 3));
    static_cast<void>(maker->expect<wire::OrderRejected>());
    EXPECT_EQ(gateway_->stats().reports_out, direct + 1);
    EXPECT_EQ(gateway_->stats().reports_recovered, 61U);
}

TEST_F(GatewayRecovery, AClientThatFallsBehindWhatIsHeldIsDisconnected) {
    static_cast<void>(fills_while_away(63));
    const auto maker = client();
    ASSERT_EQ(maker->resume(1, kEpoch, 1).next_report, 1U);
    // While it is still being sent the 64 it missed it earns itself 70 more,
    // which is more than is held. The ones it was waiting for are
    // overwritten, and there is no honest way left to carry on.
    std::array<wire::NewOrder, 70> orders{};
    for (std::size_t i = 0; i < orders.size(); ++i) {
        orders[i] = limit(9, core::Side::Buy, 950, 1, 2 + i);
    }
    maker->send_bytes(std::as_bytes(std::span{orders}));
    EXPECT_TRUE(maker->closed_by_server());
    EXPECT_EQ(gateway_->stats().slow_consumers, 1U);

    // It can come back for what is held now.
    const auto again = client();
    const wire::LoginAccepted accepted = again->resume(1, kEpoch, 1);
    EXPECT_EQ(accepted.live_from, 135U);
    EXPECT_EQ(accepted.next_report, 71U);
}

TEST_F(GatewayRecovery, RejectsMadeByTheGatewayAreHeldLikeAnyReport) {
    auto alice = client();
    alice->login(1);
    alice->send(limit(9, core::Side::Buy, 950, 1, 1));
    static_cast<void>(alice->expect<wire::OrderRejected>());
    wire::CancelOrder cancel{};
    cancel.order_id = ~std::uint64_t{0};  // names a shard that does not exist
    alice->send(cancel);
    static_cast<void>(alice->expect<wire::CancelRejected>());
    alice->close();
    wait_until_nobody_is_connected();

    alice = client();
    EXPECT_EQ(alice->resume(1, kEpoch, 1).live_from, 3U);
    const auto order = alice->expect<wire::OrderRejected>();
    EXPECT_EQ(order.reason, wire::RejectCode::UnknownSymbol);
    EXPECT_EQ(order.client_order_id, 1U);
    EXPECT_EQ(alice->expect<wire::CancelRejected>().order_id, cancel.order_id);
}

TEST_F(GatewayRecovery, AClientLearnsThatItsOrdersWereCancelledWhenItLeft) {
    cancel_on_disconnect_ = true;
    start(holding());
    auto maker = client();
    maker->login(1);
    maker->send(limit(3, core::Side::Sell, 1000, 10, 1));
    const auto resting = maker->expect<wire::OrderAccepted>();
    maker->close();
    wait_until_nobody_is_connected();
    for (int i = 0; i < 50; ++i) {
        pump();
    }

    maker = client();
    const wire::LoginAccepted accepted = maker->resume(1, kEpoch, 2);
    EXPECT_EQ(accepted.live_from, 3U);
    const auto canceled = maker->expect<wire::Canceled>();
    EXPECT_EQ(canceled.order_id, resting.order_id);
    EXPECT_EQ(canceled.reason, wire::CancelReason::Disconnected);
}

TEST_F(GatewayTest, WithNothingHeldWhatWasMissedIsGone) {
    static_cast<void>(shards_);
    auto alice = client();
    alice->login(1);
    alice->send(limit(3, core::Side::Buy, 950, 1, 1));
    static_cast<void>(alice->expect<wire::OrderAccepted>());
    alice->close();
    wait_until_nobody_is_connected();

    // Numbered all the same, so the client can tell that it missed one.
    alice = client();
    const wire::LoginAccepted accepted = alice->resume(1, gateway_->epoch(), 1);
    EXPECT_EQ(accepted.next_report, 2U);
    EXPECT_EQ(accepted.live_from, 2U);
    EXPECT_TRUE(alice->quiet());
    EXPECT_EQ(gateway_->stats().recovery_gaps, 1U);
    EXPECT_EQ(gateway_->stats().recoveries, 0U);
}

TEST_F(GatewayRecovery, OnlySoManySessionsHaveReportsHeld) {
    start(holding(kHeld, 1));
    const auto report_and_leave = [this](std::uint32_t id) {
        const auto who = client();
        who->login(id);
        who->send(limit(3, core::Side::Buy, 950, 1, 1));
        static_cast<void>(who->expect<wire::OrderAccepted>());
        who->close();
        wait_until_nobody_is_connected();
    };
    report_and_leave(1);  // takes the only room there is
    report_and_leave(2);

    const auto first = client();
    EXPECT_EQ(first->resume(1, kEpoch, 1).next_report, 1U);
    EXPECT_EQ(first->expect<wire::OrderAccepted>().client_order_id, 1U);
    const auto second = client();
    EXPECT_EQ(second->resume(2, kEpoch, 1).next_report, 2U) << "held with no room to hold it";
    EXPECT_TRUE(second->quiet());
}

TEST_F(GatewayRecovery, OneSessionsReportsAreNotAnothers) {
    static_cast<void>(fills_while_away(3));
    // Client 2 was there throughout and saw its six reports. Asked from 1
    // it gets its own six back, not client 1's four.
    const auto buyer = client();
    const wire::LoginAccepted accepted = buyer->resume(2, kEpoch, 1);
    EXPECT_EQ(accepted.live_from, 7U);
    for (std::uint64_t i = 1; i <= 3; ++i) {
        EXPECT_EQ(buyer->expect<wire::OrderAccepted>().client_order_id, i);
        EXPECT_EQ(buyer->expect<wire::Executed>().liquidity, wire::Liquidity::Taker);
    }
    EXPECT_TRUE(buyer->quiet());
}

TEST_F(GatewayTest, AnOrderSentTwiceIsCarriedOutOnce) {
    const auto alice = client();
    alice->login(1);
    alice->send(limit(3, core::Side::Buy, 950, 1, 5));
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 5U);
    alice->send(limit(3, core::Side::Buy, 950, 1, 5));  // the same again
    alice->send(limit(3, core::Side::Buy, 950, 1, 4));  // and one from before it
    alice->send(limit(3, core::Side::Buy, 950, 1, 0));
    EXPECT_TRUE(alice->quiet()) << "a repeat is not answered: the answer is in the reports";
    EXPECT_EQ(gateway_->stats().duplicate_orders, 3U);
    EXPECT_EQ(shards_[0]->stats().accepted, 1U);
    EXPECT_EQ(gateway_->stats().commands_out, 1U);

    alice->send(limit(3, core::Side::Buy, 950, 1, 6));
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 6U);
}

TEST_F(GatewayTest, AnOrderIdStaysUsedOnTheNextConnection) {
    auto alice = client();
    alice->login(1);
    alice->send(limit(3, core::Side::Buy, 950, 1, 5));
    static_cast<void>(alice->expect<wire::OrderAccepted>());
    alice->close();
    wait_until_nobody_is_connected();

    // Did the order arrive before the line dropped? The client need not
    // know. It sends it again, and the venue has it once.
    alice = client();
    EXPECT_EQ(alice->resume(1, 0, 0).last_client_order_id, 5U);
    alice->send(limit(3, core::Side::Buy, 950, 1, 5));
    EXPECT_TRUE(alice->quiet());
    EXPECT_EQ(shards_[0]->stats().accepted, 1U);
    alice->send(limit(3, core::Side::Buy, 950, 1, 6));
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 6U);

    // Another session's ids are its own business.
    const auto bob = client();
    EXPECT_EQ(bob->resume(2, 0, 0).last_client_order_id, 0U);
    bob->send(limit(3, core::Side::Buy, 950, 1, 5));
    EXPECT_EQ(bob->expect<wire::OrderAccepted>().client_order_id, 5U);
}

TEST_F(GatewayTest, AnOrderIdIsUsedEvenIfTheOrderWasRefused) {
    const auto alice = client();
    alice->login(1);
    alice->send(limit(9, core::Side::Buy, 950, 1, 1));
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::UnknownSymbol);
    wire::NewOrder nonsense = limit(3, core::Side::Buy, 950, 1, 2);
    nonsense.side = 9;
    alice->send(nonsense);
    EXPECT_EQ(alice->expect<wire::OrderRejected>().reason, wire::RejectCode::Malformed);

    // Every order with a new id gets exactly one answer, and both of these
    // have had theirs.
    alice->send(limit(3, core::Side::Buy, 950, 1, 1));
    alice->send(limit(3, core::Side::Buy, 950, 1, 2));
    EXPECT_TRUE(alice->quiet());
    EXPECT_EQ(gateway_->stats().duplicate_orders, 2U);
    EXPECT_EQ(shards_[0]->stats().accepted, 0U);
}

TEST_F(GatewayTest, AVenueToldOfEarlierOrderIdsKnowsARepeat) {
    // What a venue that has replayed its journal passes on.
    static std::array<std::uint64_t, Gateway::kMaxSessionId + 1> earlier{};
    earlier[1] = 40;
    earlier[Gateway::kMaxSessionId] = 7;
    GatewayConfig config;
    config.last_order_ids = earlier;
    start(config);

    const auto alice = client();
    EXPECT_EQ(alice->resume(1, 0, 0).last_client_order_id, 40U);
    alice->send(limit(3, core::Side::Buy, 950, 1, 40));
    EXPECT_TRUE(alice->quiet()) << "an order from before the restart was carried out again";
    alice->send(limit(3, core::Side::Buy, 950, 1, 41));
    EXPECT_EQ(alice->expect<wire::OrderAccepted>().client_order_id, 41U);

    const auto last = client();
    EXPECT_EQ(last->resume(Gateway::kMaxSessionId, 0, 0).last_client_order_id, 7U);
    const auto bob = client();
    EXPECT_EQ(bob->resume(2, 0, 0).last_client_order_id, 0U);
}

}  // namespace
}  // namespace planck::gateway
