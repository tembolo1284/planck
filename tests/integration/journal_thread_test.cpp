#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "planck/engine/shard.hpp"
#include "planck/journal/reader.hpp"
#include "planck/journal/recorder.hpp"
#include "support/fixtures.hpp"
#include "support/shard_harness.hpp"
#include "support/temp_dir.hpp"

namespace planck::journal {
namespace {

using core::Status;

// The journal the way planckd runs it: the shard on one thread, the recorder
// on another, connected by a ring, with the disk as slow as it likes. Run
// under ThreadSanitizer to check the handoff.
TEST(JournalThreads, EveryCommandReachesTheFileInOrder) {
    test::TempDir dir;
    ASSERT_TRUE(dir.ok());
    const std::uint64_t commands = test::scaled(40'000);

    engine::ShardConfig config = test::small_shard(4096, 1U << 12);
    // A small ring, so the shard really does have to wait for the recorder.
    config.journal_ring_capacity = 64;
    auto shard = std::make_unique<engine::Shard>();
    auto recorder = std::make_unique<Recorder>();
    std::array<engine::Shard*, 1> shards{shard.get()};
    const std::array<std::uint64_t, 1> fingerprints{fingerprint(config)};

    std::atomic<bool> stop_shard{false};
    std::atomic<bool> stop_recorder{false};
    std::atomic<int> stage{0};

    std::thread shard_thread([&] {
        if (shard->init(config) != Status::Ok) {
            stage.store(-1);
            return;
        }
        stage.store(1);
        shard->run(stop_shard);
    });
    while (stage.load() == 0) {
        std::this_thread::yield();
    }
    ASSERT_EQ(stage.load(), 1);

    std::thread recorder_thread([&] {
        RecorderConfig recorder_config;
        recorder_config.shards = shards;
        recorder_config.fingerprints = fingerprints;
        recorder_config.directory = dir.path();
        recorder_config.sync_interval = core::Nanos{1'000'000};
        recorder_config.idle = engine::IdlePolicy::Yield;
        if (recorder->init(recorder_config) != Status::Ok) {
            stage.store(-1);
            return;
        }
        stage.store(2);
        recorder->run(stop_recorder);
    });
    while (stage.load() == 1) {
        std::this_thread::yield();
    }
    const bool started = (stage.load() == 2);

    if (started) {
        // This thread is the gateway and the feed: it sends commands and
        // throws away what comes back.
        const auto drain = [&] {
            shard->reports().drain(1U << 16, [](const engine::Report&) noexcept {});
            shard->market().drain(1U << 16, [](const engine::MarketEvent&) noexcept {});
        };
        for (std::uint64_t i = 1; i <= commands; ++i) {
            const engine::Command command =
                test::new_order(static_cast<std::uint16_t>(1 + (i % 3)), 3,
                                (i % 2 == 0) ? core::Side::Buy : core::Side::Sell, 1000, 1, i);
            while (!shard->commands().try_push(command)) {
                drain();
            }
            drain();
        }
        // Wait for the shard to work through it all.
        for (int round = 0; round < 2'000'000 && shard->commands().size_approx() != 0; ++round) {
            drain();
            std::this_thread::yield();
        }
    }

    // Matching stops first, then the recorder empties the ring and syncs.
    stop_shard.store(true);
    shard_thread.join();
    stop_recorder.store(true);
    recorder_thread.join();
    ASSERT_TRUE(started) << "a component failed to start";

    EXPECT_FALSE(recorder->failed());
    EXPECT_EQ(shard->sequence(), commands);
    EXPECT_EQ(recorder->stats().records, commands);
    EXPECT_EQ(recorder->stats().discarded, 0U);
    EXPECT_GT(recorder->stats().syncs, 0U);
    EXPECT_EQ(recorder->durable_times().count(), commands) << "one wait timed for every record";
    EXPECT_EQ(recorder->sync_times().count(), recorder->stats().syncs);
    EXPECT_LT(recorder->stats().writes, commands) << "one write per record: nothing is being batched";

    Path path;
    ASSERT_EQ(shard_path(dir.path(), core::ShardId{2}, path), Status::Ok);
    const auto reader = std::make_unique<Reader>();
    ASSERT_EQ(reader->open(path.c_str()), Status::Ok);
    engine::JournalEntry entry;
    std::uint64_t read = 0;
    std::uint64_t last_time = 0;
    Status status = reader->next(entry);
    while (status == Status::Ok) {
        ++read;
        ASSERT_EQ(entry.sequence, read);
        ASSERT_EQ(entry.command.id, read) << "commands reached the file out of order";
        ASSERT_GE(entry.timestamp_ns, last_time);
        last_time = entry.timestamp_ns;
        status = reader->next(entry);
    }
    EXPECT_EQ(status, Status::Closed);
    EXPECT_EQ(read, commands);
}

}  // namespace
}  // namespace planck::journal
