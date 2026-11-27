#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "planck/engine/shard.hpp"
#include "planck/journal/recorder.hpp"
#include "planck/journal/replay.hpp"
#include "support/feed_auditor.hpp"
#include "support/fixtures.hpp"
#include "support/shard_harness.hpp"
#include "support/temp_dir.hpp"

namespace planck::journal {
namespace {

using core::Status;

engine::ShardConfig journaling_shard(std::uint32_t orders = 3000) {
    engine::ShardConfig config = test::small_shard(orders, 1U << 12);
    config.journal_ring_capacity = 1U << 12;
    return config;
}

bool same_bytes(const engine::MarketEvent& a, const engine::MarketEvent& b) {
    return a.length == b.length && std::memcmp(a.bytes.data(), b.bytes.data(), a.length) == 0;
}

// One life of the venue's matching side, on the test's own thread: a shard,
// the recorder writing its journal, and everything the shard published.
struct Session {
    std::unique_ptr<engine::Shard> shard = std::make_unique<engine::Shard>();
    std::unique_ptr<Recorder> recorder = std::make_unique<Recorder>();
    std::array<engine::Shard*, 1> shards{};
    std::array<std::uint64_t, 1> fingerprints{};
    std::vector<engine::MarketEvent> market;
    test::FeedAuditor auditor;
    ReplayResult replayed;
    // The auditor also checks what sessions are told against what they
    // asked for. That only works in a first life: after a restart, fills
    // arrive for orders that were sent before the auditor existed.
    bool audit_reports = true;

    // Start a shard and bring it up to date from whatever journal is there.
    Status start(const std::string& directory, const engine::ShardConfig& config,
                 core::Nanos sync_interval = core::Nanos{10'000'000}) {
        if (const Status status = shard->init(config); status != Status::Ok) {
            return status;
        }
        Path path;
        if (const Status status = shard_path(directory, shard->id(), path); status != Status::Ok) {
            return status;
        }
        fingerprints[0] = fingerprint(config);
        const Status status = replay(path.c_str(), *shard, fingerprints[0], replayed,
                                     [this]() noexcept { collect(); });
        if (status != Status::Ok) {
            return status;
        }
        audit_reports = (replayed.records == 0);
        collect();
        shards[0] = shard.get();
        RecorderConfig recorder_config;
        recorder_config.shards = shards;
        recorder_config.fingerprints = fingerprints;
        recorder_config.directory = directory;
        recorder_config.sync_interval = sync_interval;
        return recorder->init(recorder_config);
    }

    void collect() noexcept {
        shard->market().drain(1U << 20, [this](const engine::MarketEvent& e) noexcept {
            market.push_back(e);
            auditor.on_market(e);
        });
        shard->reports().drain(1U << 20, [this](const engine::Report& r) noexcept {
            if (audit_reports) {
                auditor.on_report(r);
            }
        });
    }

    void pump() {
        static_cast<void>(shard->poll());
        collect();
        static_cast<void>(recorder->poll());
    }

    // Drive random order flow through the shard.
    void trade(test::RandomFlow& flow, std::size_t commands) {
        for (std::size_t sent = 0; sent < commands && !::testing::Test::HasFailure(); ++sent) {
            const engine::Command command = flow.next(auditor);
            while (!shard->commands().try_push(command)) {
                pump();
            }
            if (sent % 7 == 0) {
                pump();
            }
        }
        while (shard->poll() != 0) {
            collect();
        }
        collect();
    }

    // A clean shutdown: everything on disk.
    void stop() {
        pump();
        recorder->finish();
    }
};

class JournalReplay : public ::testing::TestWithParam<std::uint64_t> {
protected:
    void SetUp() override { ASSERT_TRUE(dir_.ok()); }
    test::TempDir dir_;
};

TEST_P(JournalReplay, RestartRebuildsBooksAndStreamExactly) {
    const engine::ShardConfig config = journaling_shard();
    test::RandomFlow flow(GetParam());
    const std::size_t commands = test::scaled(6000);

    Session first;
    ASSERT_EQ(first.start(dir_.path(), config), Status::Ok);
    EXPECT_EQ(first.replayed.records, 0U) << "nothing to replay on a first run";
    first.trade(flow, commands);
    first.stop();
    ASSERT_FALSE(first.recorder->failed());
    ASSERT_EQ(first.recorder->stats().records, first.shard->sequence());
    ASSERT_GT(first.shard->stats().trades, 0U);

    // The process "dies" and a new one starts from the same directory.
    Session second;
    ASSERT_EQ(second.start(dir_.path(), config), Status::Ok);
    EXPECT_EQ(second.replayed.records, first.shard->sequence());
    EXPECT_FALSE(second.replayed.torn_tail);
    EXPECT_EQ(second.shard->sequence(), first.shard->sequence());
    EXPECT_EQ(second.shard->store().in_use(), first.shard->store().in_use());
    EXPECT_EQ(second.shard->stats().trades, first.shard->stats().trades);
    EXPECT_EQ(second.shard->stats().rejected, first.shard->stats().rejected);

    // The replayed market data is the first run's, byte for byte. A feed
    // subscriber cannot tell that there was a restart.
    ASSERT_EQ(second.market.size(), first.market.size());
    for (std::size_t i = 0; i < first.market.size(); ++i) {
        ASSERT_TRUE(same_bytes(second.market[i], first.market[i])) << "market message " << i;
    }
    // And the books rebuilt from that stream are the books the shard holds.
    second.auditor.expect_matches(*second.shard, test::kSymbols);

    // Life goes on after the restart, and a second restart rebuilds that too.
    test::RandomFlow more(GetParam() + 1000);
    second.trade(more, commands / 2);
    second.stop();
    ASSERT_FALSE(second.recorder->failed());

    Session third;
    ASSERT_EQ(third.start(dir_.path(), config), Status::Ok);
    EXPECT_EQ(third.replayed.records, second.shard->sequence());
    ASSERT_EQ(third.market.size(), second.market.size());
    for (std::size_t i = 0; i < second.market.size(); ++i) {
        ASSERT_TRUE(same_bytes(third.market[i], second.market[i])) << "market message " << i;
    }
    third.auditor.expect_matches(*third.shard, test::kSymbols);
    RecordProperty("commands", static_cast<int>(third.replayed.records));
    RecordProperty("journal_bytes", static_cast<int>(third.recorder->stats().bytes));
}

INSTANTIATE_TEST_SUITE_P(Seeds, JournalReplay, ::testing::Values(11, 12, 13));

class JournalRecovery : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.ok());
        ASSERT_EQ(shard_path(dir_.path(), core::ShardId{2}, path_), Status::Ok);
    }

    // A first run that leaves `commands` commands in the journal.
    void first_run(std::size_t commands) {
        Session run;
        ASSERT_EQ(run.start(dir_.path(), journaling_shard()), Status::Ok);
        test::RandomFlow flow(5);
        run.trade(flow, commands);
        run.stop();
        sequence_ = run.shard->sequence();
        resting_ = run.shard->store().in_use();
    }

    test::TempDir dir_;
    Path path_;
    std::uint64_t sequence_ = 0;
    std::uint64_t resting_ = 0;
};

TEST_F(JournalRecovery, TornTailIsDroppedAndTheRestIsKept) {
    first_run(500);
    const auto full = std::filesystem::file_size(path_.c_str());
    std::filesystem::resize_file(path_.c_str(), full - 20);  // the last record is torn

    Session run;
    ASSERT_EQ(run.start(dir_.path(), journaling_shard()), Status::Ok);
    EXPECT_TRUE(run.replayed.torn_tail);
    EXPECT_EQ(run.replayed.records, sequence_ - 1) << "everything but the torn record";
    EXPECT_EQ(run.replayed.ignored_bytes, sizeof(Record) - 20);
    EXPECT_EQ(run.recorder->stats().truncated_bytes, sizeof(Record) - 20);
    EXPECT_EQ(std::filesystem::file_size(path_.c_str()), full - sizeof(Record));

    // The journal is whole again and takes new records.
    test::RandomFlow flow(6);
    run.trade(flow, 50);
    run.stop();
    Session again;
    ASSERT_EQ(again.start(dir_.path(), journaling_shard()), Status::Ok);
    EXPECT_FALSE(again.replayed.torn_tail);
    EXPECT_EQ(again.replayed.records, run.shard->sequence());
}

TEST_F(JournalRecovery, AJournalThatEndsInALargeCloseIsReplayedToTheEnd) {
    // A market ring that holds one stage, so that cancelling this many
    // orders has to be done a piece at a time, with the ring emptied in
    // between. The close is the last thing in the journal: nothing comes
    // after it to push it through.
    engine::ShardConfig config = journaling_shard();
    config.market_ring_capacity = static_cast<std::uint32_t>(engine::Shard::kStageCapacity);
    const std::size_t orders = (5 * engine::Shard::kStageCapacity) + 9;
    {
        Session run;
        ASSERT_EQ(run.start(dir_.path(), config), Status::Ok);
        const auto send = [&run](const engine::Command& command) {
            while (!run.shard->commands().try_push(command)) {
                run.pump();
            }
        };
        for (std::size_t i = 0; i < orders; ++i) {
            run.auditor.expect_reply(i, 1, 1);
            send(test::new_order(1, 3, core::Side::Buy, 950, 1, i));
        }
        run.auditor.expect_close(1);
        send(test::session_closed(1));
        for (int i = 0; i < 1000 && run.shard->stats().disconnect_cancels != orders; ++i) {
            run.pump();
        }
        ASSERT_EQ(run.shard->stats().disconnect_cancels, orders);
        run.stop();
        ASSERT_EQ(run.shard->sequence(), orders + 1);
    }

    Session again;
    ASSERT_EQ(again.start(dir_.path(), config), Status::Ok);
    EXPECT_EQ(again.replayed.records, orders + 1);
    EXPECT_EQ(again.shard->store().in_use(), 0U) << "replay returned with the close unfinished";
    EXPECT_EQ(again.shard->stats().disconnect_cancels, orders);
    EXPECT_EQ(again.market.size(), 2 * orders) << "an add and a delete for every order";
    again.auditor.expect_matches(*again.shard, test::kSymbols);
}

TEST_F(JournalRecovery, DamageInTheMiddleStopsTheStart) {
    first_run(500);
    {
        // Flip every bit of one byte of record 101. Writing a fixed value
        // would do nothing on the rare run where the byte already held it.
        const auto where =
            static_cast<std::streamoff>(sizeof(FileHeader) + (100 * sizeof(Record)) + 9);
        std::fstream file(path_.c_str(), std::ios::in | std::ios::out | std::ios::binary);
        file.seekg(where);
        const int original = file.get();
        ASSERT_NE(original, std::char_traits<char>::eof());
        file.seekp(where);
        file.put(static_cast<char>(original ^ 0xFF));
        ASSERT_TRUE(file.good());
    }
    Session run;
    EXPECT_EQ(run.start(dir_.path(), journaling_shard()), Status::Protocol)
        << "started with a third of the history silently missing";
}

TEST_F(JournalRecovery, AJournalFromAnotherConfigurationIsRefused) {
    first_run(100);
    Session bigger;
    EXPECT_EQ(bigger.start(dir_.path(), journaling_shard(6000)), Status::InvalidArgument)
        << "a different order capacity would hand out different order ids";
}

TEST_F(JournalRecovery, RecorderRefusesAShardThatWasNotReplayed) {
    first_run(100);
    auto shard = std::make_unique<engine::Shard>();
    const engine::ShardConfig config = journaling_shard();
    ASSERT_EQ(shard->init(config), Status::Ok);  // fresh, and never brought up to date
    engine::Shard* const shards[] = {shard.get()};
    const std::uint64_t fingerprints[] = {fingerprint(config)};
    RecorderConfig recorder_config;
    recorder_config.shards = shards;
    recorder_config.fingerprints = fingerprints;
    recorder_config.directory = dir_.path();
    auto recorder = std::make_unique<Recorder>();
    EXPECT_EQ(recorder->init(recorder_config), Status::Protocol);
}

TEST_F(JournalRecovery, ReplayRefusesAShardThatHasAlreadyTraded) {
    first_run(100);
    auto shard = std::make_unique<engine::Shard>();
    const engine::ShardConfig config = journaling_shard();
    ASSERT_EQ(shard->init(config), Status::Ok);
    ASSERT_TRUE(shard->commands().try_push(test::new_order(1, 3, core::Side::Buy, 950, 1, 1)));
    ASSERT_EQ(shard->poll(), 1U);
    ReplayResult result;
    EXPECT_EQ(replay(path_.c_str(), *shard, fingerprint(config), result, []() noexcept {}),
              Status::InvalidArgument);
}

TEST_F(JournalRecovery, ReportsTheHighestSessionSeen) {
    Session run;
    ASSERT_EQ(run.start(dir_.path(), journaling_shard()), Status::Ok);
    run.audit_reports = false;  // these orders are pushed by hand, not through the flow
    for (const std::uint16_t session : {std::uint16_t{4}, std::uint16_t{311}, std::uint16_t{17}}) {
        ASSERT_TRUE(run.shard->commands().try_push(
            test::new_order(session, 3, core::Side::Buy, 950, 1, session)));
    }
    run.stop();
    Session again;
    ASSERT_EQ(again.start(dir_.path(), journaling_shard()), Status::Ok);
    EXPECT_EQ(again.replayed.records, 3U);
    EXPECT_EQ(again.replayed.max_session, 311);
}

TEST_F(JournalRecovery, SyncsAsOftenAsItWasToldTo) {
    // An interval of zero: the disk is forced after every write.
    {
        Session eager;
        ASSERT_EQ(eager.start(dir_.path(), journaling_shard(), core::Nanos{0}), Status::Ok);
        test::RandomFlow flow(8);
        eager.trade(flow, 300);
        eager.pump();
        const RecorderStats& stats = eager.recorder->stats();
        ASSERT_GT(stats.writes, 5U);
        EXPECT_EQ(stats.syncs, stats.writes);
        EXPECT_EQ(stats.bytes, stats.records * sizeof(Record));
    }
    // An interval longer than the test: nothing is forced until shutdown.
    Session lazy;
    ASSERT_EQ(lazy.start(dir_.path(), journaling_shard(), core::Nanos{3'600'000'000'000}), Status::Ok);
    test::RandomFlow flow(9);
    lazy.trade(flow, 300);
    lazy.pump();
    EXPECT_GT(lazy.recorder->stats().writes, 5U);
    EXPECT_EQ(lazy.recorder->stats().syncs, 0U) << "synced before the interval was up";
    lazy.stop();
    EXPECT_EQ(lazy.recorder->stats().syncs, 1U) << "a clean shutdown must leave everything on disk";
}

TEST(RecorderConfigTest, RejectsBadConfigurations) {
    test::TempDir dir;
    ASSERT_TRUE(dir.ok());
    auto journaled = std::make_unique<engine::Shard>();
    auto plain = std::make_unique<engine::Shard>();
    ASSERT_EQ(journaled->init(journaling_shard()), Status::Ok);
    ASSERT_EQ(plain->init(test::small_shard()), Status::Ok);
    engine::Shard* const good[] = {journaled.get()};
    engine::Shard* const no_ring[] = {plain.get()};
    const std::uint64_t one[] = {1};
    const std::uint64_t two[] = {1, 2};

    const auto init_with = [&](auto change) {
        RecorderConfig config;
        config.shards = good;
        config.fingerprints = one;
        config.directory = dir.path();
        change(config);
        const auto recorder = std::make_unique<Recorder>();
        const Status status = recorder->init(config);
        EXPECT_FALSE(recorder->initialised());
        return status;
    };
    EXPECT_EQ(init_with([](RecorderConfig& c) { c.shards = {}; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([&](RecorderConfig& c) { c.fingerprints = two; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([&](RecorderConfig& c) { c.shards = no_ring; }), Status::InvalidArgument)
        << "a shard with no journal ring has nothing to record";
    EXPECT_EQ(init_with([](RecorderConfig& c) { c.directory = ""; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](RecorderConfig& c) { c.directory = "/no/such/directory/anywhere"; }),
              Status::NotFound);
}

}  // namespace
}  // namespace planck::journal
