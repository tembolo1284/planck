#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include "planck.h"
#include "planck/engine/shard.hpp"
#include "support/fixtures.hpp"
#include "support/shard_harness.hpp"

namespace {

using planck::core::Status;

constexpr planck_symbol kSymbols[] = {
    {.id = 3, .reserved = 0, .band_low = 900, .band_ticks = 256},
    {.id = 700, .reserved = 0, .band_low = -128, .band_ticks = 256},
};

planck_config small_config(std::uint32_t orders = 256) {
    planck_config config;
    planck_config_init(&config);
    config.shard = 2;
    config.symbols = kSymbols;
    config.symbol_count = 2;
    config.order_capacity = orders;
    config.huge_pages = PLANCK_HUGE_PAGES_OFF;
    return config;
}

planck_order order_for(std::uint32_t session, std::uint8_t side, std::int32_t price,
                       std::uint32_t qty, std::uint64_t client_order_id,
                       std::uint8_t tif = PLANCK_DAY, std::uint16_t symbol = 3) {
    planck_order order{};
    order.client_order_id = client_order_id;
    order.session = session;
    order.symbol = symbol;
    order.side = side;
    order.type = PLANCK_LIMIT;
    order.price = price;
    order.qty = qty;
    order.tif = tif;
    return order;
}

class CapiTest : public ::testing::Test {
protected:
    void SetUp() override { start(small_config()); }
    void TearDown() override { planck_engine_destroy(engine_); }

    void start(const planck_config& config) {
        planck_engine_destroy(engine_);
        engine_ = nullptr;
        ASSERT_EQ(planck_engine_create(&config, &engine_), PLANCK_OK);
        ASSERT_NE(engine_, nullptr);
    }

    std::vector<planck_report> reports() {
        std::vector<planck_report> out;
        planck_report report;
        while (planck_next_report(engine_, &report) == 1) {
            out.push_back(report);
        }
        return out;
    }

    std::vector<planck_market_event> market() {
        std::vector<planck_market_event> out;
        planck_market_event event;
        while (planck_next_market_event(engine_, &event) == 1) {
            out.push_back(event);
        }
        return out;
    }

    // Enter an order that must be accepted, and return the engine's id for it.
    std::uint64_t rest(std::uint32_t session, std::uint8_t side, std::int32_t price,
                       std::uint32_t qty) {
        const planck_order order = order_for(session, side, price, qty, next_client_id_++);
        EXPECT_EQ(planck_submit(engine_, &order), PLANCK_OK);
        const std::vector<planck_report> out = reports();
        EXPECT_EQ(out.size(), 1U);
        EXPECT_EQ(out.empty() ? 0U : out[0].kind, std::uint32_t{PLANCK_REPORT_ACCEPTED});
        return out.empty() ? 0 : out[0].order_id;
    }

    planck_stats stats() {
        planck_stats out{};
        EXPECT_EQ(planck_get_stats(engine_, &out), PLANCK_OK);
        return out;
    }

    planck_engine* engine_ = nullptr;
    std::uint64_t next_client_id_ = 1;
};

TEST(CapiBasics, SaysWhatItIs) {
    EXPECT_EQ(planck_abi_version(), std::uint32_t{PLANCK_ABI_VERSION});
    EXPECT_EQ(std::string_view{planck_version()}, "0.1.0");
    EXPECT_EQ(std::string_view{planck_status_name(PLANCK_OK)}, "Ok");
    EXPECT_EQ(std::string_view{planck_status_name(PLANCK_WOULD_BLOCK)}, "WouldBlock");
    EXPECT_EQ(std::string_view{planck_status_name(PLANCK_PERMISSION + 1)}, "Unknown");
    EXPECT_EQ(std::string_view{planck_status_name(-1)}, "Unknown");
    EXPECT_EQ(std::string_view{planck_reject_name(PLANCK_REJECT_BOOK_FULL)}, "BookFull");
    EXPECT_EQ(std::string_view{planck_reject_name(1000)}, "Unknown");
    for (std::int32_t status = -2; status < 40; ++status) {
        ASSERT_NE(planck_status_name(status), nullptr);
        ASSERT_NE(planck_reject_name(static_cast<std::uint32_t>(status)), nullptr);
    }
}

TEST(CapiBasics, ConfigInitSetsEveryField) {
    planck_config config;
    std::memset(&config, 0xAB, sizeof(config));
    planck_config_init(&config);
    EXPECT_EQ(config.struct_size, sizeof(planck_config));
    EXPECT_EQ(config.shard, 0U);
    EXPECT_EQ(config.symbols, nullptr);
    EXPECT_EQ(config.symbol_count, 0U);
    EXPECT_EQ(config.order_capacity, std::uint32_t{PLANCK_DEFAULT_ORDER_CAPACITY});
    EXPECT_EQ(config.huge_pages, std::uint32_t{PLANCK_HUGE_PAGES_TRY});
    EXPECT_EQ(config.reserved, 0U);
    planck_config_init(nullptr);  // must not crash
}

TEST(CapiBasics, CreateRefusesBadConfigurations) {
    const auto create_with = [](auto change) {
        planck_config config = small_config();
        change(config);
        // A value that is obviously not an engine, to see it overwritten.
        auto* engine = reinterpret_cast<planck_engine*>(std::uintptr_t{0x1});
        const planck_status status = planck_engine_create(&config, &engine);
        EXPECT_EQ(engine, nullptr) << "a failed create must leave a null handle";
        return status;
    };
    static constexpr planck_symbol kDuplicate[] = {{3, 0, 0, 64}, {3, 0, 0, 64}};
    static constexpr planck_symbol kBadId[] = {{5000, 0, 0, 64}};
    static constexpr planck_symbol kBadBand[] = {{5, 0, 0, 100}};
    static constexpr planck_symbol kReservedSet[] = {{5, 1, 0, 64}};

    EXPECT_EQ(create_with([](planck_config& c) { c.struct_size -= 4; }), PLANCK_UNSUPPORTED);
    EXPECT_EQ(create_with([](planck_config& c) { c.struct_size = 0; }), PLANCK_UNSUPPORTED);
    EXPECT_EQ(create_with([](planck_config& c) { c.symbols = nullptr; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(create_with([](planck_config& c) { c.symbol_count = 0; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(create_with([](planck_config& c) { c.reserved = 1; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(create_with([](planck_config& c) { c.huge_pages = 3; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(create_with([](planck_config& c) { c.symbol_count = 4097; }), PLANCK_OUT_OF_RANGE);
    EXPECT_EQ(create_with([](planck_config& c) { c.shard = 16; }), PLANCK_OUT_OF_RANGE);
    EXPECT_EQ(create_with([](planck_config& c) { c.order_capacity = PLANCK_MAX_ORDER_CAPACITY + 1; }),
              PLANCK_OUT_OF_RANGE);
    EXPECT_EQ(create_with([](planck_config& c) { c.symbols = kDuplicate; }), PLANCK_DUPLICATE);
    EXPECT_EQ(create_with([](planck_config& c) { c.symbols = kBadId; c.symbol_count = 1; }),
              PLANCK_OUT_OF_RANGE);
    EXPECT_EQ(create_with([](planck_config& c) { c.symbols = kBadBand; c.symbol_count = 1; }),
              PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(create_with([](planck_config& c) { c.symbols = kReservedSet; c.symbol_count = 1; }),
              PLANCK_INVALID_ARGUMENT);

    const planck_config good = small_config();
    planck_engine* engine = nullptr;
    EXPECT_EQ(planck_engine_create(nullptr, &engine), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_engine_create(&good, nullptr), PLANCK_INVALID_ARGUMENT);
}

TEST(CapiBasics, ZeroCapacityMeansTheDefault) {
    planck_config config = small_config(0);
    planck_engine* engine = nullptr;
    ASSERT_EQ(planck_engine_create(&config, &engine), PLANCK_OK);
    planck_engine_destroy(engine);
}

TEST_F(CapiTest, EveryFunctionSurvivesNullPointers) {
    const planck_order order = order_for(1, PLANCK_BUY, 950, 1, 1);
    planck_report report;
    planck_market_event event;
    planck_quote quote;
    planck_stats stats_out;
    std::uint64_t quantity = 0;

    EXPECT_EQ(planck_submit(nullptr, &order), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_submit(engine_, nullptr), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_cancel(nullptr, 1, 1), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_next_report(nullptr, &report), 0);
    EXPECT_EQ(planck_next_market_event(nullptr, &event), 0);
    EXPECT_EQ(planck_best(nullptr, 3, PLANCK_BUY, &quote), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_best(engine_, 3, PLANCK_BUY, nullptr), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_quantity_at(nullptr, 3, 950, &quantity), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_quantity_at(engine_, 3, 950, nullptr), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_get_stats(nullptr, &stats_out), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_get_stats(engine_, nullptr), PLANCK_INVALID_ARGUMENT);
    planck_engine_destroy(nullptr);

    // Asking with nowhere to put the answer must not throw the answer away.
    ASSERT_EQ(planck_submit(engine_, &order), PLANCK_OK);
    EXPECT_EQ(planck_next_report(engine_, nullptr), 0);
    EXPECT_EQ(planck_next_market_event(engine_, nullptr), 0);
    EXPECT_EQ(reports().size(), 1U);
    EXPECT_EQ(market().size(), 1U);
}

TEST_F(CapiTest, AMalformedOrderIsTheCallersMistakeAndChangesNothing) {
    const auto submit_with = [this](auto change) {
        planck_order order = order_for(1, PLANCK_BUY, 950, 1, 1);
        change(order);
        return planck_submit(engine_, &order);
    };
    EXPECT_EQ(submit_with([](planck_order& o) { o.side = 2; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(submit_with([](planck_order& o) { o.type = 2; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(submit_with([](planck_order& o) { o.tif = 3; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(submit_with([](planck_order& o) { o.session = 0; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(submit_with([](planck_order& o) { o.session = PLANCK_MAX_SESSION + 1; }),
              PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(submit_with([](planck_order& o) { o.reserved[6] = 1; }), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_cancel(engine_, 0, 1), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_cancel(engine_, PLANCK_MAX_SESSION + 1, 1), PLANCK_INVALID_ARGUMENT);

    EXPECT_EQ(stats().commands, 0U) << "a refused call reached the engine";
    EXPECT_TRUE(reports().empty());
    EXPECT_TRUE(market().empty());

    // The largest session number is a real session.
    EXPECT_EQ(submit_with([](planck_order& o) { o.session = PLANCK_MAX_SESSION; }), PLANCK_OK);
    const std::vector<planck_report> out = reports();
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].session, std::uint32_t{PLANCK_MAX_SESSION});
}

TEST_F(CapiTest, TheMarketsRefusalsComeAsReports) {
    const auto reason_for = [this](const planck_order& order) {
        EXPECT_EQ(planck_submit(engine_, &order), PLANCK_OK);
        const std::vector<planck_report> out = reports();
        EXPECT_EQ(out.size(), 1U);
        if (out.size() != 1) {
            return std::uint32_t{999};
        }
        EXPECT_EQ(out[0].kind, std::uint32_t{PLANCK_REPORT_REJECTED});
        EXPECT_EQ(out[0].client_order_id, order.client_order_id);
        EXPECT_EQ(out[0].session, order.session);
        EXPECT_EQ(out[0].order_id, 0U);
        return out[0].reason;
    };
    EXPECT_EQ(reason_for(order_for(1, PLANCK_BUY, 950, 1, 11, PLANCK_DAY, 99)),
              std::uint32_t{PLANCK_REJECT_UNKNOWN_SYMBOL});
    EXPECT_EQ(reason_for(order_for(1, PLANCK_BUY, 5000, 1, 12)),
              std::uint32_t{PLANCK_REJECT_PRICE_OUT_OF_BAND});
    EXPECT_EQ(reason_for(order_for(1, PLANCK_BUY, 950, 0, 13)),
              std::uint32_t{PLANCK_REJECT_INVALID_QUANTITY});
    EXPECT_EQ(reason_for(order_for(1, PLANCK_SELL, 950, 5, 14, PLANCK_FOK)),
              std::uint32_t{PLANCK_REJECT_NOT_FILLABLE});
    EXPECT_TRUE(market().empty()) << "a refused order must not show on the public stream";
    EXPECT_EQ(stats().rejected, 4U);
}

TEST_F(CapiTest, ATradeIsReportedToBothSidesAndPrintedOnce) {
    const std::uint64_t first = rest(1, PLANCK_BUY, 1000, 10);
    const std::uint64_t second = rest(2, PLANCK_BUY, 1000, 4);
    EXPECT_EQ(first >> 60, 2U) << "order ids carry the shard number from the config";
    static_cast<void>(market());

    const planck_order sell = order_for(3, PLANCK_SELL, 999, 12, 77);
    ASSERT_EQ(planck_submit(engine_, &sell), PLANCK_OK);
    const std::vector<planck_report> out = reports();
    ASSERT_EQ(out.size(), 5U);
    EXPECT_EQ(out[0].kind, std::uint32_t{PLANCK_REPORT_ACCEPTED});
    EXPECT_EQ(out[0].client_order_id, 77U);
    EXPECT_GT(out[0].timestamp_ns, 1'600'000'000ULL * 1'000'000'000ULL);

    const planck_report& taker1 = out[1];
    const planck_report& maker1 = out[2];
    const planck_report& taker2 = out[3];
    const planck_report& maker2 = out[4];
    for (const planck_report* fill : {&taker1, &maker1, &taker2, &maker2}) {
        EXPECT_EQ(fill->kind, std::uint32_t{PLANCK_REPORT_EXECUTED});
        EXPECT_EQ(fill->price, 1000) << "trades happen at the resting order's price";
        EXPECT_EQ(fill->timestamp_ns, out[0].timestamp_ns);
        EXPECT_EQ(fill->client_order_id, 0U);
        EXPECT_EQ(fill->reason, 0U);
    }
    EXPECT_EQ(taker1.session, 3U);
    EXPECT_EQ(taker1.liquidity, std::uint32_t{PLANCK_TAKER});
    EXPECT_EQ(taker1.order_id, out[0].order_id);
    EXPECT_EQ(taker1.qty, 10U);
    EXPECT_EQ(taker1.leaves, 2U);
    EXPECT_EQ(maker1.session, 1U);
    EXPECT_EQ(maker1.liquidity, std::uint32_t{PLANCK_MAKER});
    EXPECT_EQ(maker1.order_id, first);
    EXPECT_EQ(maker1.leaves, 0U);
    EXPECT_EQ(maker1.match_id, taker1.match_id);
    EXPECT_EQ(taker2.qty, 2U);
    EXPECT_EQ(taker2.leaves, 0U);
    EXPECT_EQ(maker2.session, 2U);
    EXPECT_EQ(maker2.order_id, second);
    EXPECT_EQ(maker2.leaves, 2U);
    EXPECT_EQ(taker2.match_id, taker1.match_id + 1);

    const std::vector<planck_market_event> prints = market();
    ASSERT_EQ(prints.size(), 2U);
    EXPECT_EQ(prints[0].kind, std::uint32_t{PLANCK_MARKET_TRADE});
    EXPECT_EQ(prints[0].symbol, 3);
    EXPECT_EQ(prints[0].order_id, first);
    EXPECT_EQ(prints[0].match_id, taker1.match_id);
    EXPECT_EQ(prints[0].qty, 10U);
    EXPECT_EQ(prints[0].price, 1000);
    EXPECT_EQ(prints[1].order_id, second);
    EXPECT_EQ(prints[1].qty, 2U);

    planck_quote quote;
    ASSERT_EQ(planck_best(engine_, 3, PLANCK_BUY, &quote), PLANCK_OK);
    EXPECT_EQ(quote.valid, 1U);
    EXPECT_EQ(quote.price, 1000);
    EXPECT_EQ(quote.qty, 2U);
    ASSERT_EQ(planck_best(engine_, 3, PLANCK_SELL, &quote), PLANCK_OK);
    EXPECT_EQ(quote.valid, 0U);
    EXPECT_EQ(quote.price, 0);
    EXPECT_EQ(quote.qty, 0U);
}

TEST_F(CapiTest, OnlyTheOwnerCanCancel) {
    const planck_order order = order_for(5, PLANCK_SELL, -100, 3, 9, PLANCK_DAY, 700);
    ASSERT_EQ(planck_submit(engine_, &order), PLANCK_OK);
    std::vector<planck_report> out = reports();
    ASSERT_EQ(out.size(), 1U);
    ASSERT_EQ(out[0].kind, std::uint32_t{PLANCK_REPORT_ACCEPTED});
    const std::uint64_t resting = out[0].order_id;
    static_cast<void>(market());

    ASSERT_EQ(planck_cancel(engine_, 6, resting), PLANCK_OK);
    out = reports();
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].kind, std::uint32_t{PLANCK_REPORT_CANCEL_REJECTED});
    EXPECT_EQ(out[0].session, 6U);
    EXPECT_EQ(out[0].order_id, resting);
    EXPECT_EQ(out[0].reason, std::uint32_t{PLANCK_REJECT_UNKNOWN_ORDER});
    EXPECT_TRUE(market().empty());

    std::uint64_t quantity = 0;
    ASSERT_EQ(planck_quantity_at(engine_, 700, -100, &quantity), PLANCK_OK);
    EXPECT_EQ(quantity, 3U) << "the order must still be there";

    ASSERT_EQ(planck_cancel(engine_, 5, resting), PLANCK_OK);
    out = reports();
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].kind, std::uint32_t{PLANCK_REPORT_CANCELED});
    EXPECT_EQ(out[0].session, 5U);
    EXPECT_EQ(out[0].order_id, resting);
    EXPECT_EQ(out[0].qty, 3U);
    EXPECT_EQ(out[0].reason, std::uint32_t{PLANCK_CANCEL_REQUESTED});
    const std::vector<planck_market_event> events = market();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, std::uint32_t{PLANCK_MARKET_DELETE});
    EXPECT_EQ(events[0].symbol, 700);
    EXPECT_EQ(events[0].order_id, resting);
}

TEST_F(CapiTest, BookQueriesKnowTheirSymbols) {
    planck_quote quote{.price = 5, .valid = 5, .qty = 5};
    std::uint64_t quantity = 5;
    EXPECT_EQ(planck_best(engine_, 4, PLANCK_BUY, &quote), PLANCK_NOT_FOUND);
    EXPECT_EQ(quote.valid, 0U) << "output must be cleared even on failure";
    EXPECT_EQ(planck_best(engine_, 60000, PLANCK_BUY, &quote), PLANCK_NOT_FOUND);
    EXPECT_EQ(planck_best(engine_, 3, 2, &quote), PLANCK_INVALID_ARGUMENT);
    EXPECT_EQ(planck_quantity_at(engine_, 4, 950, &quantity), PLANCK_NOT_FOUND);
    EXPECT_EQ(quantity, 0U);
    quantity = 5;
    EXPECT_EQ(planck_quantity_at(engine_, 3, 100'000, &quantity), PLANCK_OK);
    EXPECT_EQ(quantity, 0U) << "a price outside the band holds nothing";
}

// The engine runs on the caller's thread, so nothing empties its output
// queues while it matches. If they could not hold everything one order can
// produce, the engine would wait forever for room. The worst order is one
// that takes out a full book.
TEST_F(CapiTest, OneOrderCanSweepAFullBook) {
    // Enough orders that the queues are sized by this case and not by the
    // slack that is added on top of it.
    constexpr std::uint32_t kOrders = 3000;
    start(small_config(kOrders));
    std::size_t accepted = 0;
    std::size_t added = 0;
    for (std::uint32_t i = 0; i < kOrders; ++i) {
        const planck_order order =
            order_for(1 + (i % 5), PLANCK_SELL, static_cast<std::int32_t>(1000 + (i % 40)), 1, i);
        ASSERT_EQ(planck_submit(engine_, &order), PLANCK_OK);
        accepted += reports().size();
        added += market().size();
    }
    ASSERT_EQ(accepted, kOrders);
    ASSERT_EQ(added, kOrders);
    // The book is full: one more is refused by the market, not by the call.
    const planck_order extra = order_for(1, PLANCK_SELL, 1000, 1, 9999);
    ASSERT_EQ(planck_submit(engine_, &extra), PLANCK_OK);
    std::vector<planck_report> out = reports();
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out.back().kind, std::uint32_t{PLANCK_REPORT_REJECTED});
    EXPECT_EQ(out.back().reason, std::uint32_t{PLANCK_REJECT_BOOK_FULL});
    ASSERT_EQ(stats().resting, kOrders);

    const planck_order sweep = order_for(9, PLANCK_BUY, 1100, kOrders + 5, 5000, PLANCK_IOC);
    ASSERT_EQ(planck_submit(engine_, &sweep), PLANCK_OK);
    out = reports();
    ASSERT_EQ(out.size(), 1U + (2U * kOrders) + 1U) << "acceptance, two fills per order, and a cancel";
    EXPECT_EQ(out.front().kind, std::uint32_t{PLANCK_REPORT_ACCEPTED});
    EXPECT_EQ(out.back().kind, std::uint32_t{PLANCK_REPORT_CANCELED});
    EXPECT_EQ(out.back().reason, std::uint32_t{PLANCK_CANCEL_UNFILLED});
    EXPECT_EQ(out.back().qty, 5U);
    std::int32_t last_price = 0;
    for (std::uint32_t i = 0; i < kOrders; ++i) {
        const planck_report& taker = out[1 + (2 * i)];
        ASSERT_EQ(taker.kind, std::uint32_t{PLANCK_REPORT_EXECUTED});
        ASSERT_EQ(taker.leaves, kOrders + 5 - (i + 1));
        ASSERT_GE(taker.price, last_price) << "a buy takes the cheapest offers first";
        last_price = taker.price;
    }
    EXPECT_EQ(market().size(), kOrders);
    EXPECT_EQ(stats().resting, 0U);
}

TEST_F(CapiTest, UnreadOutputEventuallyHoldsNewOrdersBack) {
    // Never read anything. Every order leaves a report behind, and every
    // order that rests leaves a market event as well.
    std::uint64_t calls = 0;
    planck_status status = PLANCK_OK;
    for (; calls < 100'000 && status == PLANCK_OK; ++calls) {
        const planck_order order = order_for(1, PLANCK_BUY, 950, 1, calls);
        status = planck_submit(engine_, &order);
    }
    ASSERT_EQ(status, PLANCK_WOULD_BLOCK) << "the engine kept going with its output unread";
    const planck_stats before = stats();
    EXPECT_EQ(before.commands, calls - 1) << "the refused call must not have reached the engine";
    EXPECT_GE(before.commands, 1000U) << "a caller should be able to leave a thousand unread";

    // Still refused, and still harmless, until something is read.
    const planck_order again = order_for(1, PLANCK_BUY, 950, 1, 777);
    EXPECT_EQ(planck_submit(engine_, &again), PLANCK_WOULD_BLOCK);
    EXPECT_EQ(planck_cancel(engine_, 1, 12345), PLANCK_WOULD_BLOCK);
    EXPECT_EQ(stats().commands, before.commands);

    // Nothing was lost while it was held back.
    const std::vector<planck_report> out = reports();
    ASSERT_EQ(out.size(), before.commands);
    for (std::size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(out[i].client_order_id, i);
    }
    EXPECT_EQ(market().size(), before.accepted);
    EXPECT_EQ(planck_submit(engine_, &again), PLANCK_OK);
}

// The engine must stop taking orders while there is not room for the worst
// thing the next one could do, and it must judge each queue on its own. A
// caller who reads only reports, or only market events, has to be held back
// just the same. With 3000 orders of capacity each queue has room for fewer
// than 3000 unread entries on top of a full-book sweep, so 3000 orders with
// one side unread must hit the limit.
TEST_F(CapiTest, HoldsBackWhenEitherQueueAloneIsTooFull) {
    constexpr std::uint32_t kOrders = 3000;
    for (const bool read_reports : {true, false}) {
        start(small_config(kOrders));
        planck_status status = PLANCK_OK;
        std::uint32_t sent = 0;
        for (; sent < kOrders && status == PLANCK_OK; ++sent) {
            const planck_order order = order_for(1, PLANCK_SELL, 1000, 1, sent);
            status = planck_submit(engine_, &order);
            if (read_reports) {
                static_cast<void>(reports());
            } else {
                static_cast<void>(market());
            }
        }
        ASSERT_EQ(status, PLANCK_WOULD_BLOCK)
            << "took " << sent << " orders with the " << (read_reports ? "market events" : "reports")
            << " unread. A sweep of the book now would have nowhere to put its output.";
        EXPECT_GE(sent, 1000U) << "held back long before it had to be";

        // The limit is exact. Reading a single entry from the neglected
        // queue is enough to be let in again, and what is let in must then
        // fit, even if it is the worst case: one order taking the whole
        // book, with everything else still unread.
        const std::uint64_t resting = stats().resting;
        planck_report one_report;
        planck_market_event one_event;
        const std::size_t already_read = 1;
        if (read_reports) {
            ASSERT_EQ(planck_next_market_event(engine_, &one_event), 1);
        } else {
            ASSERT_EQ(planck_next_report(engine_, &one_report), 1);
        }
        const planck_order sweep = order_for(2, PLANCK_BUY, 1000, kOrders, 1, PLANCK_IOC);
        ASSERT_EQ(planck_submit(engine_, &sweep), PLANCK_OK);
        const std::size_t unread_reports = read_reports ? 0 : (sent - 1 - already_read);
        const std::size_t unread_events = read_reports ? (sent - 1 - already_read) : 0;
        EXPECT_EQ(reports().size(), unread_reports + 1U + (2U * resting) + 1U);
        EXPECT_EQ(market().size(), unread_events + resting);
    }
}

// The tightest fit there is: the book is full, the report queue has been
// left as full as the engine allows, and one order then takes every resting
// order and has some left over. That order produces an acceptance, two fills
// for each of 3000 orders, and a cancel, into exactly the room that remains.
TEST_F(CapiTest, AFullBookSweepFitsWhenTheQueueIsAsFullAsAllowed) {
    constexpr std::uint32_t kOrders = 3000;
    start(small_config(kOrders));
    for (std::uint32_t i = 0; i < kOrders; ++i) {
        const planck_order order = order_for(1, PLANCK_SELL, 1000, 1, i);
        ASSERT_EQ(planck_submit(engine_, &order), PLANCK_OK);
        ASSERT_EQ(reports().size(), 1U);
        ASSERT_EQ(market().size(), 1U);
    }
    ASSERT_EQ(stats().resting, kOrders);

    // Orders the market refuses leave one report each and change nothing.
    const planck_order refused = order_for(1, PLANCK_BUY, 5000, 1, 1);
    std::size_t unread = 0;
    planck_status status = planck_submit(engine_, &refused);
    for (; status == PLANCK_OK && unread < 100'000; status = planck_submit(engine_, &refused)) {
        ++unread;
    }
    ASSERT_EQ(status, PLANCK_WOULD_BLOCK);
    planck_report one;
    ASSERT_EQ(planck_next_report(engine_, &one), 1);
    --unread;

    const planck_order sweep = order_for(2, PLANCK_BUY, 1000, kOrders + 5, 2, PLANCK_IOC);
    ASSERT_EQ(planck_submit(engine_, &sweep), PLANCK_OK);
    const std::vector<planck_report> out = reports();
    ASSERT_EQ(out.size(), unread + 1U + (2U * kOrders) + 1U);
    EXPECT_EQ(out.back().kind, std::uint32_t{PLANCK_REPORT_CANCELED});
    EXPECT_EQ(out.back().qty, 5U);
    EXPECT_EQ(market().size(), kOrders);
    EXPECT_EQ(stats().resting, 0U);
}

// The same for the public side, where the worst order is again one that
// takes the whole book: a trade print for each of 3000 orders. (An order
// that would rest what it has left cannot do more. It needs a slot of its
// own before it may match, so it cannot enter a book that is full.)
TEST_F(CapiTest, AFullBookSweepFitsWhenTheMarketQueueIsAsFullAsAllowed) {
    constexpr std::uint32_t kOrders = 3000;
    start(small_config(kOrders));
    std::vector<std::uint64_t> ids;
    for (std::uint32_t i = 0; i < kOrders; ++i) {
        ids.push_back(rest(1, PLANCK_SELL, 1000, 1));
        ASSERT_EQ(market().size(), 1U);
    }

    // Cancel an order and put one back, over and over, reading the reports
    // and never the market events. The book stays full while the public
    // queue fills up.
    std::size_t unread = 0;
    planck_status status = PLANCK_OK;
    for (std::size_t round = 0; round < 100'000 && status == PLANCK_OK; ++round) {
        status = planck_cancel(engine_, 1, ids.back());
        if (status != PLANCK_OK) {
            break;
        }
        ids.pop_back();
        ++unread;
        static_cast<void>(reports());
        const planck_order order = order_for(1, PLANCK_SELL, 1000, 1, round);
        status = planck_submit(engine_, &order);
        if (status == PLANCK_OK) {
            ++unread;
            const std::vector<planck_report> out = reports();
            ASSERT_EQ(out.size(), 1U);
            ids.push_back(out[0].order_id);
        }
    }
    ASSERT_EQ(status, PLANCK_WOULD_BLOCK);

    // Read one event at a time, only as many as it takes to be let in.
    const auto until_accepted = [&](const planck_order& order) {
        for (int reads = 0; reads < 10; ++reads) {
            if (planck_submit(engine_, &order) == PLANCK_OK) {
                return true;
            }
            planck_market_event event;
            EXPECT_EQ(planck_next_market_event(engine_, &event), 1);
            --unread;
        }
        return false;
    };
    if (ids.size() < kOrders) {
        ASSERT_TRUE(until_accepted(order_for(1, PLANCK_SELL, 1000, 1, 1)));
        ++unread;
        static_cast<void>(reports());
    }
    ASSERT_EQ(stats().resting, kOrders);

    ASSERT_TRUE(until_accepted(order_for(2, PLANCK_BUY, 1000, kOrders + 5, 2, PLANCK_IOC)));
    const std::vector<planck_market_event> events = market();
    ASSERT_EQ(events.size(), unread + kOrders);
    EXPECT_EQ(events.back().kind, std::uint32_t{PLANCK_MARKET_TRADE});
    EXPECT_EQ(reports().size(), 1U + (2U * kOrders) + 1U);
    EXPECT_EQ(stats().resting, 0U);
    EXPECT_EQ(stats().trades, kOrders);
}

TEST_F(CapiTest, TwoEnginesShareNothing) {
    planck_config other_config = small_config();
    other_config.shard = 7;
    planck_engine* other = nullptr;
    ASSERT_EQ(planck_engine_create(&other_config, &other), PLANCK_OK);

    const std::uint64_t here = rest(1, PLANCK_BUY, 950, 5);
    planck_quote quote;
    ASSERT_EQ(planck_best(other, 3, PLANCK_BUY, &quote), PLANCK_OK);
    EXPECT_EQ(quote.valid, 0U);

    const planck_order order = order_for(1, PLANCK_BUY, 960, 5, 1);
    ASSERT_EQ(planck_submit(other, &order), PLANCK_OK);
    planck_report report;
    ASSERT_EQ(planck_next_report(other, &report), 1);
    EXPECT_EQ(report.order_id >> 60, 7U);
    EXPECT_NE(report.order_id, here);
    EXPECT_TRUE(reports().empty()) << "one engine's report turned up in the other";

    // An id from one engine means nothing to the other.
    ASSERT_EQ(planck_cancel(other, 1, here), PLANCK_OK);
    ASSERT_EQ(planck_next_report(other, &report), 1);
    EXPECT_EQ(report.kind, std::uint32_t{PLANCK_REPORT_CANCEL_REJECTED});
    planck_engine_destroy(other);
}

// Random flow through the C interface and, command for command, through a
// Shard driven directly. Apart from the clock, which each reads for itself,
// everything that comes out must be the same.
TEST_F(CapiTest, MatchesTheEngineItWraps) {
    planck::engine::ShardConfig direct_config = planck::test::small_shard(256);
    const auto direct = std::make_unique<planck::engine::Shard>();
    ASSERT_EQ(direct->init(direct_config), Status::Ok);

    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    const auto random = [&state](std::uint32_t bound) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::uint32_t>((state >> 33) % bound);
    };

    // Orders accepted so far and who entered them. Many have since traded
    // or been cancelled, which is wanted: cancels must fail as well as work.
    struct Known {
        std::uint64_t id;
        std::uint16_t owner;
    };
    std::vector<Known> known;
    std::uint64_t compared_reports = 0;
    std::uint64_t compared_events = 0;
    const std::size_t commands = planck::test::scaled(20'000);
    for (std::size_t i = 0; i < commands && !HasFailure(); ++i) {
        auto session = static_cast<std::uint16_t>(1 + random(4));
        if (random(4) == 0 && !known.empty()) {
            // One of the last few dozen, usually as its owner.
            const auto recent = static_cast<std::uint32_t>(std::min<std::size_t>(known.size(), 48));
            const Known& target = known[known.size() - 1 - random(recent)];
            const std::uint64_t id = target.id;
            session = (random(5) == 0) ? session : target.owner;
            ASSERT_EQ(planck_cancel(engine_, session, id), PLANCK_OK);
            ASSERT_TRUE(direct->commands().try_push(planck::test::cancel_order(session, id)));
        } else {
            const bool second_symbol = random(3) == 0;
            const auto symbol = static_cast<std::uint16_t>(second_symbol ? 700 : 3);
            const std::int32_t low = second_symbol ? -128 : 900;
            // Mostly inside the band, sometimes just outside it.
            const std::int32_t price = low - 2 + static_cast<std::int32_t>(random(260));
            const auto side = static_cast<std::uint8_t>(random(2));
            const auto tif = static_cast<std::uint8_t>(random(3));
            const std::uint32_t qty = random(30);  // zero now and then
            const planck_order order = order_for(session, side, price, qty, i, tif, symbol);
            ASSERT_EQ(planck_submit(engine_, &order), PLANCK_OK);
            ASSERT_TRUE(direct->commands().try_push(planck::test::new_order(
                session, symbol, static_cast<planck::core::Side>(side), price, qty, i,
                static_cast<planck::core::TimeInForce>(tif))));
        }
        ASSERT_EQ(direct->poll(), 1U);

        const std::vector<planck_report> got = reports();
        std::size_t index = 0;
        direct->reports().drain(1U << 20, [&](const planck::engine::Report& want) noexcept {
            if (index >= got.size()) {
                ++index;
                return;
            }
            const planck_report& have = got[index++];
            EXPECT_EQ(have.session, want.session.raw());
            switch (planck::test::type_of(want)) {
                case planck::wire::MsgType::OrderAccepted: {
                    const auto message = planck::test::as<planck::wire::OrderAccepted>(want);
                    EXPECT_EQ(have.kind, std::uint32_t{PLANCK_REPORT_ACCEPTED});
                    EXPECT_EQ(have.client_order_id, message.client_order_id);
                    EXPECT_EQ(have.order_id, message.order_id);
                    known.push_back({message.order_id, want.session.raw()});
                    break;
                }
                case planck::wire::MsgType::OrderRejected: {
                    const auto message = planck::test::as<planck::wire::OrderRejected>(want);
                    EXPECT_EQ(have.kind, std::uint32_t{PLANCK_REPORT_REJECTED});
                    EXPECT_EQ(have.client_order_id, message.client_order_id);
                    EXPECT_EQ(have.reason, std::to_underlying(message.reason));
                    break;
                }
                case planck::wire::MsgType::Executed: {
                    const auto message = planck::test::as<planck::wire::Executed>(want);
                    EXPECT_EQ(have.kind, std::uint32_t{PLANCK_REPORT_EXECUTED});
                    EXPECT_EQ(have.order_id, message.order_id);
                    EXPECT_EQ(have.match_id, message.match_id);
                    EXPECT_EQ(have.price, message.price);
                    EXPECT_EQ(have.qty, message.qty);
                    EXPECT_EQ(have.leaves, message.leaves);
                    EXPECT_EQ(have.liquidity, std::to_underlying(message.liquidity));
                    break;
                }
                case planck::wire::MsgType::Canceled: {
                    const auto message = planck::test::as<planck::wire::Canceled>(want);
                    EXPECT_EQ(have.kind, std::uint32_t{PLANCK_REPORT_CANCELED});
                    EXPECT_EQ(have.order_id, message.order_id);
                    EXPECT_EQ(have.qty, message.qty);
                    EXPECT_EQ(have.reason, std::to_underlying(message.reason));
                    break;
                }
                case planck::wire::MsgType::CancelRejected: {
                    const auto message = planck::test::as<planck::wire::CancelRejected>(want);
                    EXPECT_EQ(have.kind, std::uint32_t{PLANCK_REPORT_CANCEL_REJECTED});
                    EXPECT_EQ(have.order_id, message.order_id);
                    EXPECT_EQ(have.reason, std::to_underlying(message.reason));
                    break;
                }
                default:
                    ADD_FAILURE() << "the shard sent a report this test does not know";
            }
            ++compared_reports;
        });
        ASSERT_EQ(got.size(), index) << "a different number of reports at command " << i;

        const std::vector<planck_market_event> events = market();
        index = 0;
        direct->market().drain(1U << 20, [&](const planck::engine::MarketEvent& want) noexcept {
            if (index >= events.size()) {
                ++index;
                return;
            }
            const planck_market_event& have = events[index++];
            switch (planck::test::type_of(want)) {
                case planck::wire::MsgType::MdAddOrder: {
                    const auto message = planck::test::as<planck::wire::MdAddOrder>(want);
                    EXPECT_EQ(have.kind, std::uint32_t{PLANCK_MARKET_ADD});
                    EXPECT_EQ(have.symbol, message.symbol_id);
                    EXPECT_EQ(have.side, message.side);
                    EXPECT_EQ(have.order_id, message.order_id);
                    EXPECT_EQ(have.price, message.price);
                    EXPECT_EQ(have.qty, message.qty);
                    break;
                }
                case planck::wire::MsgType::MdExecuted: {
                    const auto message = planck::test::as<planck::wire::MdExecuted>(want);
                    EXPECT_EQ(have.kind, std::uint32_t{PLANCK_MARKET_TRADE});
                    EXPECT_EQ(have.symbol, message.symbol_id);
                    EXPECT_EQ(have.order_id, message.order_id);
                    EXPECT_EQ(have.match_id, message.match_id);
                    EXPECT_EQ(have.price, message.price);
                    EXPECT_EQ(have.qty, message.qty);
                    break;
                }
                case planck::wire::MsgType::MdDeleted: {
                    const auto message = planck::test::as<planck::wire::MdDeleted>(want);
                    EXPECT_EQ(have.kind, std::uint32_t{PLANCK_MARKET_DELETE});
                    EXPECT_EQ(have.symbol, message.symbol_id);
                    EXPECT_EQ(have.order_id, message.order_id);
                    break;
                }
                default:
                    ADD_FAILURE() << "the shard published an event this test does not know";
            }
            ++compared_events;
        });
        ASSERT_EQ(events.size(), index) << "a different number of market events at command " << i;
    }

    const planck_stats ours = stats();
    const planck::engine::ShardStats& theirs = direct->stats();
    EXPECT_EQ(ours.commands, theirs.commands);
    EXPECT_EQ(ours.accepted, theirs.accepted);
    EXPECT_EQ(ours.rejected, theirs.rejected);
    EXPECT_EQ(ours.trades, theirs.trades);
    EXPECT_EQ(ours.cancels, theirs.cancels);
    EXPECT_EQ(ours.cancel_rejects, theirs.cancel_rejects);
    EXPECT_EQ(ours.resting, direct->store().in_use());
    // The run must have exercised something of everything.
    EXPECT_GT(theirs.trades, commands / 20);
    EXPECT_GT(theirs.cancels, commands / 100);
    EXPECT_GT(theirs.rejected, commands / 100);
    EXPECT_GT(compared_reports, commands);
    EXPECT_GT(compared_events, commands / 4);

    for (const planck_symbol& symbol : kSymbols) {
        const planck::book::OrderBook* const book = direct->book(planck::core::SymbolId{symbol.id});
        ASSERT_NE(book, nullptr);
        for (const std::uint8_t side : {std::uint8_t{PLANCK_BUY}, std::uint8_t{PLANCK_SELL}}) {
            planck_quote quote;
            ASSERT_EQ(planck_best(engine_, symbol.id, side, &quote), PLANCK_OK);
            const planck::book::Quote want = book->best(static_cast<planck::core::Side>(side));
            EXPECT_EQ(quote.valid != 0, want.valid);
            if (want.valid) {
                EXPECT_EQ(quote.price, want.price.raw());
                EXPECT_EQ(quote.qty, want.qty);
            }
        }
        for (std::int32_t tick = 0; tick < static_cast<std::int32_t>(symbol.band_ticks); ++tick) {
            std::uint64_t quantity = 0;
            const std::int32_t price = symbol.band_low + tick;
            ASSERT_EQ(planck_quantity_at(engine_, symbol.id, price, &quantity), PLANCK_OK);
            ASSERT_EQ(quantity, book->quantity_at(planck::core::Price{price})) << "at price " << price;
        }
    }
}

TEST_F(CapiTest, TimestampsAreWallClockAndNeverGoBackwards) {
    std::uint64_t previous = 0;
    for (std::uint64_t i = 0; i < 500; ++i) {
        const planck_order order = order_for(1, PLANCK_BUY, 950, 1, i, PLANCK_IOC);
        ASSERT_EQ(planck_submit(engine_, &order), PLANCK_OK);
        const std::vector<planck_report> out = reports();
        ASSERT_EQ(out.size(), 2U);
        ASSERT_GE(out[0].timestamp_ns, previous);
        previous = out[0].timestamp_ns;
    }
    EXPECT_GT(previous, 1'600'000'000ULL * 1'000'000'000ULL);
}

}  // namespace
