#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <ostream>

#include "planck/engine/shard.hpp"
#include "support/feed_auditor.hpp"
#include "support/fixtures.hpp"
#include "support/shard_harness.hpp"

namespace planck::engine {
namespace {

using core::Status;

struct Audit {
    const char* name;
    std::uint64_t seed;
    // Small enough that the store fills and BookFull becomes common.
    std::uint32_t order_capacity;
    std::size_t commands;
};

void PrintTo(const Audit& audit, std::ostream* out) {
    *out << audit.name;
}

// Drives a shard with random commands on the test's own thread and has a
// FeedAuditor check every message it emits. At checkpoints the books the
// auditor rebuilt from market data alone must equal the shard's real books.
class ShardAudit : public ::testing::TestWithParam<Audit> {};

TEST_P(ShardAudit, EveryMessageIsConsistent) {
    const Audit& audit = GetParam();
    const std::size_t commands = test::scaled(audit.commands);

    const auto shard = std::make_unique<Shard>();
    ASSERT_EQ(shard->init(test::small_shard(audit.order_capacity, 1U << 12)), Status::Ok);

    test::FeedAuditor auditor;
    test::RandomFlow flow(audit.seed);
    const auto pump = [&] {
        static_cast<void>(shard->poll());
        shard->market().drain(4096, [&](const MarketEvent& e) noexcept { auditor.on_market(e); });
        shard->reports().drain(4096, [&](const Report& r) noexcept { auditor.on_report(r); });
    };
    const auto settle = [&] {
        for (int i = 0; i < 1000; ++i) {
            pump();
        }
    };

    for (std::size_t sent = 0; sent < commands; ++sent) {
        const Command command = flow.next(auditor);
        while (!shard->commands().try_push(command)) {
            pump();
        }
        // A close must leave nothing behind. Let the shard carry it out and
        // look at once, before the session sends anything more.
        if (command.kind == CommandKind::SessionClosed ||
            command.kind == CommandKind::AllSessionsClosed) {
            for (int i = 0; i < 4; ++i) {
                pump();
            }
            ASSERT_EQ(shard->stats().commands, sent + 1) << "the close has not been handled";
            if (command.kind == CommandKind::SessionClosed) {
                auditor.expect_nothing_live_for(command.session.raw());
            } else {
                auditor.expect_nothing_live();
                EXPECT_EQ(shard->store().in_use(), 0U);
            }
        }
        if (sent % 7 == 0) {
            pump();
        }
        if (sent % 10'000 == 0) {
            settle();
            auditor.expect_views_agree();
            auditor.expect_matches(*shard, test::kSymbols);
        }
        if (::testing::Test::HasFailure()) {
            FAIL() << "stopping at command " << sent;
        }
    }

    settle();
    EXPECT_EQ(shard->stats().commands, commands);
    EXPECT_EQ(auditor.unanswered(), 0U) << "some orders were never accepted or rejected";
    auditor.expect_views_agree();
    auditor.expect_matches(*shard, test::kSymbols);

    const ShardStats& stats = shard->stats();
    EXPECT_GT(stats.trades, 0U);
    EXPECT_GT(stats.cancels, 0U);
    EXPECT_GT(stats.rejected, 0U);
    EXPECT_GT(stats.cancel_rejects, 0U);
    EXPECT_GT(stats.session_closes, 0U);
    EXPECT_GT(stats.disconnect_cancels, 0U) << "no close ever found an order to cancel";
    EXPECT_EQ(stats.disconnect_cancels, auditor.disconnect_cancels());
    RecordProperty("trades", static_cast<int>(stats.trades));
    RecordProperty("resting_at_end", static_cast<int>(auditor.resting_orders()));
}

INSTANTIATE_TEST_SUITE_P(Flows, ShardAudit,
                         ::testing::Values(Audit{"RoomyStore", 1, 6000, 60'000},
                                           Audit{"CrampedStore", 2, 900, 60'000},
                                           Audit{"AnotherSeed", 3, 3000, 60'000}),
                         ::testing::PrintToStringParamName());

}  // namespace
}  // namespace planck::engine
