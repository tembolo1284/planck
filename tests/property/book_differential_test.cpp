#include <gtest/gtest.h>

#include <cstdint>
#include <ostream>
#include <random>
#include <vector>

#include "planck/book/order_book.hpp"
#include "planck/mem/arena.hpp"
#include "support/fixtures.hpp"
#include "support/reference_book.hpp"

namespace planck::book {
namespace {

using core::OrderId;
using core::OrderType;
using core::Price;
using core::Qty;
using core::Side;
using core::Status;
using core::TimeInForce;

struct Flow {
    const char* name;
    std::uint64_t seed;
    std::uint32_t band_ticks;
    std::uint32_t store_capacity;
    // Prices are drawn within this many ticks of the middle of the band.
    std::int32_t spread;
    // Share of orders priced to rest instead of cross.
    unsigned passive_percent;
    unsigned cancel_percent;
    std::size_t operations;
};

void PrintTo(const Flow& flow, std::ostream* out) {
    *out << flow.name;
}

// Runs the same random order flow through book::OrderBook and through the
// naive ReferenceBook, and requires them to agree after every operation on
// the outcome, on each fill, on both best quotes and on the number of
// resting orders. Depth at every price is compared periodically.
//
// Two books share one store, as they do inside a shard.
class BookDifferential : public ::testing::TestWithParam<Flow> {};

TEST_P(BookDifferential, AgreesWithTheReferenceBook) {
    const Flow& flow = GetParam();
    const std::size_t operations = test::scaled(flow.operations);
    const std::int32_t lows[2] = {-static_cast<std::int32_t>(flow.band_ticks / 2), 50'000};

    mem::Arena arena;
    ASSERT_EQ(arena.init(std::size_t{96} << 20, sys::HugePages::Off), Status::Ok);
    OrderStore store;
    ASSERT_EQ(store.init(arena.allocate_array<Order>(flow.store_capacity), core::ShardId{3}), Status::Ok);
    OrderBook books[2];
    std::vector<test::ReferenceBook> references;
    for (std::size_t i = 0; i < 2; ++i) {
        const BookConfig config{core::SymbolId{static_cast<std::uint16_t>(i)}, Price{lows[i]}, flow.band_ticks};
        ASSERT_EQ(books[i].init(config, store, arena), Status::Ok);
        references.emplace_back(lows[i], flow.band_ticks);
    }

    struct Known {
        std::uint64_t id;
        std::size_t book;
    };
    std::vector<Known> known;  // ids we may try to cancel. Some are already gone.
    std::mt19937_64 rng(flow.seed);
    test::TradeLog log;

    for (std::size_t op = 0; op < operations; ++op) {
        SCOPED_TRACE(::testing::Message() << "operation " << op);
        const std::size_t which = rng() % 2;
        const auto roll = static_cast<unsigned>(rng() % 100);

        if (roll < flow.cancel_percent && !known.empty()) {
            const std::size_t pick = rng() % known.size();
            const Known target = known[pick];
            known[pick] = known.back();
            known.pop_back();
            // One time in twenty, send the cancel to the wrong book.
            const std::size_t via = (rng() % 20 == 0) ? (1 - target.book) : target.book;

            const test::ReferenceCancel expected =
                (via == target.book) ? references[via].cancel(target.id) : test::ReferenceCancel{};
            const CancelResult actual = books[via].cancel(OrderId{target.id});
            ASSERT_EQ(actual.reject == RejectReason::None, expected.found);
            if (expected.found) {
                ASSERT_EQ(actual.cancelled.raw(), expected.qty);
                ASSERT_EQ(actual.price.raw(), expected.price);
                ASSERT_EQ(actual.side, expected.side);
            }
            if (via != target.book) {
                known.push_back(target);
            }
        } else if (roll < flow.cancel_percent + 2) {
            // A made-up id must never resolve, and must never crash.
            ASSERT_EQ(books[which].cancel(OrderId{rng() | (std::uint64_t{0xF} << 60)}).reject,
                      RejectReason::UnknownOrder);
        } else {
            const bool buy = (rng() % 2 == 0);
            const std::int32_t mid = lows[which] + static_cast<std::int32_t>(flow.band_ticks / 2);
            const auto offset = static_cast<std::int32_t>(rng() % static_cast<std::uint64_t>(flow.spread));
            std::int32_t price = (rng() % 100 < flow.passive_percent)
                                     ? (buy ? mid - 1 - offset : mid + 1 + offset)
                                     : mid + static_cast<std::int32_t>(rng() % 13) - 6;
            if (rng() % 200 == 0) {
                // Band edges, and a little past them.
                price = lows[which] + static_cast<std::int32_t>(rng() % (flow.band_ticks + 40)) - 20;
            }
            std::uint32_t qty = (rng() % 300 == 0) ? 0 : static_cast<std::uint32_t>(1 + rng() % 100);
            if (rng() % 50 == 0) {
                qty *= 40;
            }
            const auto kind = static_cast<unsigned>(rng() % 100);
            const NewOrder order{
                buy ? Side::Buy : Side::Sell,
                (kind >= 94) ? OrderType::Market : OrderType::Limit,
                (kind < 70) ? TimeInForce::Day
                            : (kind < 86) ? TimeInForce::ImmediateOrCancel : TimeInForce::FillOrKill,
                Price{price},
                Qty{qty},
                static_cast<std::uint32_t>(rng() % 1000),
            };

            const bool room = (references[0].live() + references[1].live()) < flow.store_capacity;
            const test::ReferenceAdd expected = references[which].add(order, room);
            log.trades.clear();
            const AddResult actual = books[which].add(order, log);

            ASSERT_EQ(actual.reject, expected.reject)
                << "got " << to_string(actual.reject) << ", want " << to_string(expected.reject);
            if (expected.reject != RejectReason::None) {
                ASSERT_TRUE(log.trades.empty());
                ASSERT_EQ(actual.id, core::kNoOrderId);
            } else {
                ASSERT_NE(actual.id, core::kNoOrderId);
                ASSERT_EQ(actual.filled.raw(), expected.filled);
                ASSERT_EQ(actual.resting.raw(), expected.resting);
                ASSERT_EQ(log.trades.size(), expected.fills.size());
                for (std::size_t i = 0; i < expected.fills.size(); ++i) {
                    const Trade& got = log.trades[i];
                    const test::ReferenceFill& want = expected.fills[i];
                    ASSERT_EQ(got.resting.raw(), want.resting_id) << "fill " << i << ": wrong order";
                    ASSERT_EQ(got.price.raw(), want.price) << "fill " << i;
                    ASSERT_EQ(got.qty.raw(), want.qty) << "fill " << i;
                    ASSERT_EQ(got.resting_leaves.raw(), want.resting_leaves) << "fill " << i;
                    ASSERT_EQ(got.resting_owner, want.resting_owner) << "fill " << i;
                    ASSERT_EQ(got.aggressor, actual.id);
                    ASSERT_EQ(got.aggressor_side, order.side);
                }
                if (expected.resting != 0) {
                    references[which].rest(actual.id.raw(), order, expected.resting);
                    known.push_back(Known{actual.id.raw(), which});
                }
            }
        }

        ASSERT_EQ(store.in_use(), references[0].live() + references[1].live());
        for (std::size_t i = 0; i < 2; ++i) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                const Quote got = books[i].best(side);
                const Quote want = references[i].best(side);
                ASSERT_EQ(got.valid, want.valid);
                if (want.valid) {
                    ASSERT_EQ(got.price, want.price);
                    ASSERT_EQ(got.qty, want.qty);
                }
            }
            const Quote bid = books[i].best(Side::Buy);
            const Quote ask = books[i].best(Side::Sell);
            if (bid.valid && ask.valid) {
                ASSERT_LT(bid.price, ask.price) << "the book is crossed";
            }
        }

        if (op % 2000 == 0) {
            for (std::size_t i = 0; i < 2; ++i) {
                const std::int32_t high = lows[i] + static_cast<std::int32_t>(flow.band_ticks);
                for (std::int32_t price = lows[i] - 2; price < high + 2; ++price) {
                    ASSERT_EQ(books[i].quantity_at(Price{price}), references[i].quantity_at(price))
                        << "book " << i << " price " << price;
                }
            }
        }
        if (op % 64 == 0) {
            std::erase_if(known, [&](const Known& k) { return !references[k.book].has(k.id); });
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Flows, BookDifferential,
    ::testing::Values(
        //   name               seed  ticks   store  spread passive cancel  ops
        Flow{"TinyAndBusy",        1,     64,     48,     6,     0,    28, 40'000},
        Flow{"TinyAndFull",        5,     64,     48,     6,    85,    10, 40'000},
        Flow{"SmallCrossing",      2,    256,   4096,    10,     0,    28, 40'000},
        Flow{"SmallDeep",          6,    256,   4096,   100,    80,    15, 40'000},
        Flow{"MediumChurn",        9,   4096,   3000,  1500,    75,    20, 30'000},
        Flow{"WideAndDeep",        7,   8192, 100000,  3000,    90,     5, 20'000},
        Flow{"WidestBand",         8, 262144,  60000, 100000,   90,     5, 10'000}),
    ::testing::PrintToStringParamName());

}  // namespace
}  // namespace planck::book
