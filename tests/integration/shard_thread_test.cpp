#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "planck/engine/shard.hpp"
#include "support/feed_auditor.hpp"
#include "support/fixtures.hpp"
#include "support/shard_harness.hpp"

namespace planck::engine {
namespace {

using core::Status;

// The shard runs on its own thread through Shard::run(), as it will in the
// real process. This thread plays gateway and feed at once: it produces
// commands and consumes both output rings, with a FeedAuditor checking every
// message. Run under ThreadSanitizer to check the ring handoffs.
class ShardThread : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(ShardThread, SurvivesAnAuditedRandomRun) {
    const std::size_t commands = test::scaled(60'000);
    const auto shard = std::make_unique<Shard>();
    const ShardConfig config = test::small_shard(GetParam() % 2 == 0 ? 900 : 6000, 1U << 12);

    std::atomic<bool> stop{false};
    std::atomic<int> ready{0};  // 0 starting, 1 running, -1 failed
    std::thread shard_thread([&] {
        // init() runs here, on the thread that will own the memory.
        if (shard->init(config) != Status::Ok) {
            ready.store(-1);
            return;
        }
        ready.store(1);
        shard->run(stop);
    });
    while (ready.load() == 0) {
        std::this_thread::yield();
    }
    if (ready.load() < 0) {
        shard_thread.join();
        FAIL() << "shard failed to initialise";
    }

    test::FeedAuditor auditor;
    test::RandomFlow flow(GetParam());
    const auto pump = [&] {
        shard->market().drain(4096, [&](const MarketEvent& e) noexcept { auditor.on_market(e); });
        shard->reports().drain(4096, [&](const Report& r) noexcept { auditor.on_report(r); });
    };

    for (std::size_t sent = 0; sent < commands && !::testing::Test::HasFailure(); ++sent) {
        const Command command = flow.next(auditor);
        while (!shard->commands().try_push(command)) {
            pump();
            std::this_thread::yield();
        }
        if (sent % 7 == 0) {
            pump();
        }
    }

    // Wait until the shard has gone quiet: nothing queued and no new output
    // for a while. The stats counters are not read here, because the shard
    // thread is still writing them.
    std::uint64_t last_seen = 0;
    int quiet_rounds = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (quiet_rounds < 200 && std::chrono::steady_clock::now() < deadline) {
        pump();
        const bool idle = (auditor.messages_seen() == last_seen) && (shard->commands().size_approx() == 0);
        quiet_rounds = idle ? quiet_rounds + 1 : 0;
        last_seen = auditor.messages_seen();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    stop.store(true);
    shard_thread.join();
    pump();

    // The shard thread has exited, so its state is safe to read now.
    EXPECT_EQ(shard->stats().commands, commands);
    EXPECT_EQ(auditor.unanswered(), 0U);
    auditor.expect_views_agree();
    auditor.expect_matches(*shard, test::kSymbols);
    EXPECT_GT(shard->stats().trades, 0U);
}

INSTANTIATE_TEST_SUITE_P(Seeds, ShardThread, ::testing::Values(3U, 4U));

}  // namespace
}  // namespace planck::engine
