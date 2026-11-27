#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include <unistd.h>

#include "planck/engine/shard.hpp"
#include "planck/feed/publisher.hpp"
#include "planck/feed/receiver.hpp"
#include "planck/gateway/gateway.hpp"
#include "support/fixtures.hpp"
#include "support/shard_harness.hpp"
#include "support/wire_client.hpp"

namespace planck {
namespace {

using core::Status;

// Everything running the way planckd will run it: a shard thread, a gateway
// thread and a publisher thread, connected only by rings, with clients on
// TCP and a subscriber on UDP. Run under ThreadSanitizer to check every
// handoff between the three.
//
// Each component is initialised on the thread that runs it, as it must be.
TEST(FullStack, OrdersInFillsOutAndPrintsOnTheFeed) {
    const std::uint32_t rounds = static_cast<std::uint32_t>(test::scaled(1500));

    feed::Receiver receiver;
    feed::ReceiverConfig receiver_config;
    receiver_config.listen = {net::kLoopback, 0};
    receiver_config.stream_id = 2;
    ASSERT_EQ(receiver.init(receiver_config), Status::Ok);
    net::Endpoint feed_address;
    ASSERT_EQ(receiver.local_endpoint(feed_address), Status::Ok);

    auto shard = std::make_unique<engine::Shard>();
    auto gateway = std::make_unique<gateway::Gateway>();
    auto publisher = std::make_unique<feed::Publisher>();
    std::array<engine::Shard*, 1> shards{shard.get()};
    const std::array<gateway::Route, 1> routes{{{core::SymbolId{3}, 0}}};
    const std::array<net::Endpoint, 1> destinations{feed_address};

    std::atomic<bool> stop{false};
    std::atomic<int> stage{0};  // counts components that are up. Negative on failure.
    net::Endpoint order_entry;

    std::thread shard_thread([&] {
        if (shard->init(test::small_shard(8192, 1U << 12)) != Status::Ok) {
            stage.store(-1);
            return;
        }
        stage.store(1);
        shard->run(stop);
    });
    while (stage.load() == 0) {
        std::this_thread::yield();
    }
    ASSERT_EQ(stage.load(), 1) << "shard failed to start";

    std::thread gateway_thread([&] {
        gateway::GatewayConfig config;
        config.listen = {net::kLoopback, 0};
        config.shards = shards;
        config.routes = routes;
        config.idle = engine::IdlePolicy::Yield;
        if (gateway->init(config) != Status::Ok || gateway->local_endpoint(order_entry) != Status::Ok) {
            stage.store(-1);
            return;
        }
        stage.store(2);
        gateway->run(stop);
    });
    std::thread publisher_thread([&] {
        feed::PublisherConfig config;
        config.shards = shards;
        config.destinations = destinations;
        config.retransmit_listen = {net::kLoopback, 0};
        config.idle = engine::IdlePolicy::Yield;
        while (stage.load() == 1) {
            std::this_thread::yield();
        }
        if (stage.load() < 0 || publisher->init(config) != Status::Ok) {
            stage.store(-1);
            return;
        }
        stage.store(3);
        publisher->run(stop);
    });
    while (stage.load() > 0 && stage.load() < 3) {
        std::this_thread::yield();
    }
    const bool started = (stage.load() == 3);

    std::uint64_t prints = 0;
    std::uint64_t adds = 0;
    std::uint64_t feed_qty = 0;
    std::uint64_t next_feed_sequence = 1;
    bool feed_in_order = true;
    const auto on_feed = [&](std::uint64_t sequence, std::span<const std::byte> message) noexcept {
        feed_in_order = feed_in_order && (sequence == next_feed_sequence);
        ++next_feed_sequence;
        const wire::Frame frame = wire::next_frame(message);
        if (frame.type == wire::MsgType::MdExecuted) {
            wire::MdExecuted print{};
            if (wire::decode(frame.bytes, print) == Status::Ok) {
                ++prints;
                feed_qty += print.qty;
            }
        } else if (frame.type == wire::MsgType::MdAddOrder) {
            ++adds;
        }
    };

    if (started) {
        test::WireClient maker([&] { static_cast<void>(receiver.poll(on_feed)); });
        test::WireClient taker([&] { static_cast<void>(receiver.poll(on_feed)); });
        maker.connect(order_entry);
        taker.connect(order_entry);
        EXPECT_NE(maker.login(1), 0);
        EXPECT_NE(taker.login(2), 0);

        std::uint64_t maker_qty = 0;
        std::uint64_t taker_qty = 0;
        for (std::uint32_t round = 0; round < rounds && !::testing::Test::HasFailure(); ++round) {
            const std::int32_t price = 1000 + static_cast<std::int32_t>(round % 50);
            const std::uint32_t qty = 1 + (round % 7);

            wire::NewOrder sell{};
            sell.symbol_id = 3;
            sell.side = std::to_underlying(core::Side::Sell);
            sell.price = price;
            sell.qty = qty;
            sell.client_order_id = round + 1;
            maker.send(sell);
            const auto resting = maker.expect<wire::OrderAccepted>();
            ASSERT_EQ(resting.client_order_id, round + 1);

            wire::NewOrder buy = sell;
            buy.side = std::to_underlying(core::Side::Buy);
            taker.send(buy);
            static_cast<void>(taker.expect<wire::OrderAccepted>());
            const auto taker_fill = taker.expect<wire::Executed>();
            const auto maker_fill = maker.expect<wire::Executed>();

            ASSERT_EQ(maker_fill.order_id, resting.order_id);
            ASSERT_EQ(maker_fill.match_id, taker_fill.match_id);
            ASSERT_EQ(maker_fill.qty, qty);
            ASSERT_EQ(taker_fill.price, price);
            maker_qty += maker_fill.qty;
            taker_qty += taker_fill.qty;
        }
        EXPECT_EQ(maker_qty, taker_qty);

        // Let the feed catch up with what the clients have already been told.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (prints < rounds && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(receiver.poll(on_feed));
            ::usleep(100);
        }
        EXPECT_EQ(prints, rounds) << "trades reported to clients never appeared on the feed";
        EXPECT_EQ(adds, rounds);
        EXPECT_EQ(feed_qty, taker_qty) << "public and private quantities disagree";
        EXPECT_TRUE(feed_in_order);
        EXPECT_EQ(receiver.stats().lost, 0U);
    }

    stop.store(true);
    shard_thread.join();
    gateway_thread.join();
    publisher_thread.join();
    ASSERT_TRUE(started) << "a component failed to start";

    // The threads have stopped, so their state is safe to read.
    EXPECT_EQ(shard->stats().trades, rounds);
    EXPECT_EQ(shard->store().in_use(), 0U);
    EXPECT_EQ(gateway->stats().commands_out, std::uint64_t{rounds} * 2);
    EXPECT_EQ(gateway->stats().reports_unsent, 0U);
    EXPECT_EQ(gateway->stats().protocol_errors, 0U);
    EXPECT_EQ(publisher->stats().messages, std::uint64_t{rounds} * 2);
}

}  // namespace
}  // namespace planck
