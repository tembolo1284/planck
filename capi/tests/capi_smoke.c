/*
 * A C program that uses planck through planck.h and nothing else. That it
 * compiles as strict C11 with every warning on is half the test. The other
 * half is the short trading session below.
 */
#include <stdio.h>
#include <string.h>

#include "planck.h"

_Static_assert(sizeof(planck_order) == 32, "planck_order layout");
_Static_assert(sizeof(planck_report) == 64, "planck_report layout");
_Static_assert(sizeof(planck_market_event) == 40, "planck_market_event layout");
_Static_assert(sizeof(planck_quote) == 16, "planck_quote layout");

static int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static planck_order limit_order(uint32_t session, uint8_t side, int32_t price, uint32_t qty,
                                uint64_t client_order_id) {
    planck_order order;
    memset(&order, 0, sizeof(order));
    order.client_order_id = client_order_id;
    order.session = session;
    order.symbol = 1;
    order.side = side;
    order.type = PLANCK_LIMIT;
    order.price = price;
    order.qty = qty;
    order.tif = PLANCK_DAY;
    return order;
}

/* Read every waiting report into `reports`. Returns how many there were. */
static int read_reports(planck_engine* engine, planck_report* reports, int capacity) {
    int count = 0;
    planck_report report;
    while (planck_next_report(engine, &report) == 1) {
        if (count < capacity) {
            reports[count] = report;
        }
        ++count;
    }
    return count;
}

int main(void) {
    CHECK(planck_abi_version() == PLANCK_ABI_VERSION);
    CHECK(strlen(planck_version()) >= 5);
    CHECK(strcmp(planck_status_name(PLANCK_OK), "Ok") == 0);
    CHECK(strcmp(planck_status_name(-1), "Unknown") == 0);

    const planck_symbol symbol = {.id = 1, .band_low = 1000, .band_ticks = 1024};
    planck_config config;
    planck_config_init(&config);
    config.symbols = &symbol;
    config.symbol_count = 1;
    config.order_capacity = 256;
    config.huge_pages = PLANCK_HUGE_PAGES_OFF;

    planck_engine* engine = NULL;
    CHECK(planck_engine_create(&config, &engine) == PLANCK_OK);
    if (engine == NULL) {
        fprintf(stderr, "no engine, giving up\n");
        return 1;
    }

    /* Session 7 offers 10 at 1500. */
    planck_order sell = limit_order(7, PLANCK_SELL, 1500, 10, 41);
    CHECK(planck_submit(engine, &sell) == PLANCK_OK);

    planck_report reports[8];
    CHECK(read_reports(engine, reports, 8) == 1);
    CHECK(reports[0].kind == PLANCK_REPORT_ACCEPTED);
    CHECK(reports[0].session == 7);
    CHECK(reports[0].client_order_id == 41);
    const uint64_t resting_id = reports[0].order_id;
    CHECK(resting_id != 0);

    planck_quote quote;
    CHECK(planck_best(engine, 1, PLANCK_SELL, &quote) == PLANCK_OK);
    CHECK(quote.valid == 1 && quote.price == 1500 && quote.qty == 10);
    CHECK(planck_best(engine, 1, PLANCK_BUY, &quote) == PLANCK_OK);
    CHECK(quote.valid == 0);

    /* Session 9 lifts 4 of them. */
    planck_order buy = limit_order(9, PLANCK_BUY, 1500, 4, 42);
    buy.tif = PLANCK_IOC;
    CHECK(planck_submit(engine, &buy) == PLANCK_OK);
    CHECK(read_reports(engine, reports, 8) == 3);
    CHECK(reports[0].kind == PLANCK_REPORT_ACCEPTED && reports[0].session == 9);
    CHECK(reports[1].kind == PLANCK_REPORT_EXECUTED && reports[1].session == 9);
    CHECK(reports[1].liquidity == PLANCK_TAKER && reports[1].qty == 4 && reports[1].price == 1500);
    CHECK(reports[2].kind == PLANCK_REPORT_EXECUTED && reports[2].session == 7);
    CHECK(reports[2].liquidity == PLANCK_MAKER && reports[2].leaves == 6);
    CHECK(reports[2].order_id == resting_id);
    CHECK(reports[1].match_id == reports[2].match_id && reports[1].match_id != 0);

    /* The public record: one add, one trade. */
    planck_market_event event;
    CHECK(planck_next_market_event(engine, &event) == 1);
    CHECK(event.kind == PLANCK_MARKET_ADD && event.order_id == resting_id && event.qty == 10);
    CHECK(event.symbol == 1 && event.side == PLANCK_SELL && event.price == 1500);
    CHECK(planck_next_market_event(engine, &event) == 1);
    CHECK(event.kind == PLANCK_MARKET_TRADE && event.order_id == resting_id && event.qty == 4);
    CHECK(planck_next_market_event(engine, &event) == 0);

    /* Somebody else cannot cancel it. Its owner can. */
    CHECK(planck_cancel(engine, 9, resting_id) == PLANCK_OK);
    CHECK(read_reports(engine, reports, 8) == 1);
    CHECK(reports[0].kind == PLANCK_REPORT_CANCEL_REJECTED);
    CHECK(reports[0].reason == PLANCK_REJECT_UNKNOWN_ORDER);
    CHECK(planck_cancel(engine, 7, resting_id) == PLANCK_OK);
    CHECK(read_reports(engine, reports, 8) == 1);
    CHECK(reports[0].kind == PLANCK_REPORT_CANCELED && reports[0].qty == 6);
    CHECK(reports[0].reason == PLANCK_CANCEL_REQUESTED);

    uint64_t quantity = 99;
    CHECK(planck_quantity_at(engine, 1, 1500, &quantity) == PLANCK_OK && quantity == 0);

    planck_stats stats;
    CHECK(planck_get_stats(engine, &stats) == PLANCK_OK);
    CHECK(stats.commands == 4 && stats.trades == 1 && stats.cancels == 1 && stats.resting == 0);

    /* Bad input is refused, not crashed on. */
    CHECK(planck_submit(engine, NULL) == PLANCK_INVALID_ARGUMENT);
    CHECK(planck_submit(NULL, &sell) == PLANCK_INVALID_ARGUMENT);
    sell.side = 2;
    CHECK(planck_submit(engine, &sell) == PLANCK_INVALID_ARGUMENT);

    planck_engine_destroy(engine);
    planck_engine_destroy(NULL);

    if (failures != 0) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
