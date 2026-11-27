/*
 * planck.h: the planck matching engine as a plain C library.
 *
 * One engine is one set of order books and the thread that calls it. There
 * are no threads, sockets or files behind this interface: you hand the
 * engine an order, it matches the order before the call returns, and you
 * read back what happened.
 *
 *     planck_symbol symbol = {.id = 1, .band_low = 0, .band_ticks = 4096};
 *     planck_config config;
 *     planck_config_init(&config);
 *     config.symbols = &symbol;
 *     config.symbol_count = 1;
 *
 *     planck_engine* engine = NULL;
 *     if (planck_engine_create(&config, &engine) != PLANCK_OK) { ... }
 *
 *     planck_order order = {.client_order_id = 1, .session = 7, .symbol = 1,
 *                           .side = PLANCK_BUY, .price = 2000, .qty = 10};
 *     planck_submit(engine, &order);
 *
 *     planck_report report;
 *     while (planck_next_report(engine, &report)) { ... }
 *     planck_market_event event;
 *     while (planck_next_market_event(engine, &event)) { ... }
 *
 *     planck_engine_destroy(engine);
 *
 * Two kinds of output come back. Reports are private: each is addressed to
 * the session that owns the order it describes. Market events are public:
 * they are what a market data feed would carry, and replaying them rebuilds
 * the book.
 *
 * Threads. An engine may be used by one thread at a time. Different engines
 * are independent and may be used from different threads at once.
 *
 * Errors. Every function that can fail returns a planck_status. No function
 * aborts or throws on bad input, including null pointers. A status says
 * whether the call itself was acceptable. Whether the market accepted an
 * order is a different question, answered by a report.
 *
 * Compatibility. Every struct here has a fixed layout with no hidden
 * padding. planck_abi_version() changes if any layout or meaning does.
 */
#ifndef PLANCK_H
#define PLANCK_H

/* PLANCK_CFFI: for feeding this header to a tool that parses declarations
 * only, such as Python's cffi after `cc -E -P -DPLANCK_CFFI`. */
#if !defined(PLANCK_CFFI)
#include <stddef.h>
#include <stdint.h>
#endif

#if defined(PLANCK_CFFI)
#define PLANCK_API
#elif defined(PLANCK_BUILDING_SHARED)
#define PLANCK_API __attribute__((visibility("default")))
#else
#define PLANCK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { PLANCK_ABI_VERSION = 1 };

/* Limits. */
enum {
    PLANCK_MAX_SYMBOL_ID = 4095,
    PLANCK_MAX_SESSION = 65535,
    PLANCK_MAX_SHARD = 15,
    PLANCK_MIN_BAND_TICKS = 64,
    PLANCK_DEFAULT_ORDER_CAPACITY = 65536,
    PLANCK_MAX_ORDER_CAPACITY = 4000000
};

/* Whether a call was acceptable. Zero is success. */
typedef enum planck_status {
    PLANCK_OK = 0,
    PLANCK_INVALID_ARGUMENT = 1, /* a null pointer or a value outside its range */
    PLANCK_OUT_OF_RANGE = 2,
    PLANCK_CAPACITY = 3,   /* not enough memory */
    PLANCK_NOT_FOUND = 4,  /* no such symbol */
    PLANCK_DUPLICATE = 5,  /* the same symbol listed twice */
    PLANCK_WOULD_BLOCK = 6, /* too much unread output: read some and call again */
    PLANCK_CLOSED = 7,
    PLANCK_IO = 8,
    PLANCK_PROTOCOL = 9,
    PLANCK_UNSUPPORTED = 10, /* header and library disagree about a struct */
    PLANCK_PERMISSION = 11
} planck_status;

enum { PLANCK_BUY = 0, PLANCK_SELL = 1 };
enum { PLANCK_LIMIT = 0, PLANCK_MARKET = 1 };

/* How long an order may wait. */
enum {
    PLANCK_DAY = 0, /* rest on the book until it trades or is cancelled */
    PLANCK_IOC = 1, /* trade what it can at once, cancel the rest */
    PLANCK_FOK = 2  /* trade in full at once or not at all */
};

/* Why the market refused an order or a cancel. */
enum {
    PLANCK_REJECT_NONE = 0,
    PLANCK_REJECT_INVALID_QUANTITY = 1,  /* quantity was zero */
    PLANCK_REJECT_PRICE_OUT_OF_BAND = 2, /* price outside the symbol's band */
    PLANCK_REJECT_NOT_FILLABLE = 3,      /* fill-or-kill could not fill in full */
    PLANCK_REJECT_BOOK_FULL = 4,         /* no room for another resting order */
    PLANCK_REJECT_UNKNOWN_ORDER = 5,     /* not resting, or not this session's */
    PLANCK_REJECT_UNKNOWN_SYMBOL = 6
};

/* Why an order left the book without being fully filled. */
enum {
    PLANCK_CANCEL_REQUESTED = 0, /* its owner cancelled it */
    PLANCK_CANCEL_UNFILLED = 1   /* the rest of an IOC or market order */
};

enum { PLANCK_MAKER = 0, PLANCK_TAKER = 1 };

/* What a planck_report says. */
enum {
    PLANCK_REPORT_ACCEPTED = 1,       /* the order is in. order_id is the engine's name for it */
    PLANCK_REPORT_REJECTED = 2,       /* the order was refused. See reason */
    PLANCK_REPORT_EXECUTED = 3,       /* one fill */
    PLANCK_REPORT_CANCELED = 4,       /* the order left the book. See reason */
    PLANCK_REPORT_CANCEL_REJECTED = 5 /* a cancel was refused. See reason */
};

/* What a planck_market_event says. */
enum {
    PLANCK_MARKET_ADD = 1,   /* an order came to rest */
    PLANCK_MARKET_TRADE = 2, /* a resting order traded, in part or in full */
    PLANCK_MARKET_DELETE = 3 /* a resting order was cancelled */
};

enum { PLANCK_HUGE_PAGES_OFF = 0, PLANCK_HUGE_PAGES_TRY = 1, PLANCK_HUGE_PAGES_REQUIRE = 2 };

/* One tradable symbol. The book accepts prices from band_low up to, but not
 * including, band_low + band_ticks. band_ticks must be a power of two and
 * at least PLANCK_MIN_BAND_TICKS. Each tick in the band costs 16 bytes. */
typedef struct planck_symbol {
    uint16_t id; /* 0 to PLANCK_MAX_SYMBOL_ID */
    uint16_t reserved;
    int32_t band_low;
    uint32_t band_ticks;
} planck_symbol;

/* Fill in with planck_config_init(), then change what you need. */
typedef struct planck_config {
    /* sizeof(planck_config) as the caller was compiled. Set by
     * planck_config_init(). Lets the library notice a mismatched header. */
    uint32_t struct_size;
    /* 0 to PLANCK_MAX_SHARD. Stamped into the top bits of every order id
     * and match id, so engines given different numbers never issue the same
     * id. */
    uint32_t shard;
    /* Read during planck_engine_create() only. */
    const planck_symbol* symbols;
    uint32_t symbol_count;
    /* Most orders that can rest at once, across all symbols. Memory is up
     * to about 400 bytes for each, taken once at creation: 25 MB at the
     * default. Zero means PLANCK_DEFAULT_ORDER_CAPACITY. */
    uint32_t order_capacity;
    uint32_t huge_pages; /* PLANCK_HUGE_PAGES_* */
    uint32_t reserved;
} planck_config;

/* A new order. Zero the struct first: reserved bytes must be zero. */
typedef struct planck_order {
    /* Yours. Echoed in the ACCEPTED or REJECTED report and not otherwise
     * looked at. */
    uint64_t client_order_id;
    /* Who owns the order, 1 to PLANCK_MAX_SESSION. Reports about the order
     * carry this number, and only this number may cancel it. */
    uint32_t session;
    uint16_t symbol;
    uint8_t side; /* PLANCK_BUY or PLANCK_SELL */
    uint8_t type; /* PLANCK_LIMIT or PLANCK_MARKET */
    int32_t price; /* in ticks. Ignored for a market order */
    uint32_t qty;
    uint8_t tif; /* PLANCK_DAY, PLANCK_IOC or PLANCK_FOK */
    uint8_t reserved[7];
} planck_order;

/* Something that happened to one session's order. Fields not listed for a
 * kind are zero.
 *
 *   ACCEPTED         client_order_id, order_id, timestamp_ns
 *   REJECTED         client_order_id, reason
 *   EXECUTED         order_id, match_id, price, qty, leaves, liquidity,
 *                    timestamp_ns. Both sides of a trade get one, with the
 *                    same match_id. leaves of zero means the order is done.
 *   CANCELED         order_id, qty (the amount removed), reason, timestamp_ns
 *   CANCEL_REJECTED  order_id, reason
 */
typedef struct planck_report {
    uint32_t kind;    /* PLANCK_REPORT_* */
    uint32_t session; /* who this is for */
    uint64_t client_order_id;
    uint64_t order_id;
    uint64_t match_id;
    uint64_t timestamp_ns; /* since the Unix epoch */
    int32_t price;
    uint32_t qty;
    uint32_t leaves;
    uint32_t reason;    /* PLANCK_REJECT_* or PLANCK_CANCEL_* */
    uint32_t liquidity; /* PLANCK_MAKER or PLANCK_TAKER */
    uint32_t reserved;
} planck_report;

/* Something everyone may know. Fields not listed for a kind are zero.
 *
 *   ADD     symbol, side, order_id, price, qty, timestamp_ns
 *   TRADE   symbol, order_id (the resting order that was hit), match_id,
 *           price, qty, timestamp_ns. An order whose quantity reaches zero
 *           this way is gone: no DELETE follows.
 *   DELETE  symbol, order_id, timestamp_ns
 */
typedef struct planck_market_event {
    uint32_t kind; /* PLANCK_MARKET_* */
    uint16_t symbol;
    uint8_t side;
    uint8_t reserved;
    uint64_t order_id;
    uint64_t match_id;
    uint64_t timestamp_ns;
    int32_t price;
    uint32_t qty;
} planck_market_event;

/* The best price on one side and the total quantity resting there. */
typedef struct planck_quote {
    int32_t price;
    uint32_t valid; /* zero if that side of the book is empty */
    uint64_t qty;
} planck_quote;

typedef struct planck_stats {
    uint64_t commands;       /* orders and cancels handled */
    uint64_t accepted;
    uint64_t rejected;
    uint64_t trades;
    uint64_t cancels;
    uint64_t cancel_rejects;
    uint64_t resting;        /* orders on the books right now */
} planck_stats;

typedef struct planck_engine planck_engine;

/* PLANCK_ABI_VERSION as the library was built. Compare it with the constant
 * in the header you compiled against. */
PLANCK_API uint32_t planck_abi_version(void);
/* For example "0.1.0". Never null. */
PLANCK_API const char* planck_version(void);
/* A short name for a status or a reject reason, for messages. Never null:
 * "Unknown" for a value that is not one. */
PLANCK_API const char* planck_status_name(int32_t status);
PLANCK_API const char* planck_reject_name(uint32_t reason);

/* Set every field to its default. A null pointer is ignored. */
PLANCK_API void planck_config_init(planck_config* config);

/* Build an engine. Takes all the memory it will ever use and blocks for
 * about 50 ms while it calibrates its clock. On success *out is the engine.
 * On failure *out is null. */
PLANCK_API planck_status planck_engine_create(const planck_config* config, planck_engine** out);
/* A null pointer is ignored. */
PLANCK_API void planck_engine_destroy(planck_engine* engine);

/* Enter an order. The order has been matched by the time this returns, and
 * the reports and market events it caused are waiting to be read.
 *
 * PLANCK_INVALID_ARGUMENT means the struct itself was not acceptable (a
 * side that is neither buy nor sell, a session of zero, a reserved byte
 * set) and nothing happened. Everything else, including an unknown symbol
 * or a price outside the band, is PLANCK_OK followed by a REJECTED report.
 *
 * PLANCK_WOULD_BLOCK means too much earlier output is still unread. Nothing
 * happened. Read reports and market events, then call again. It cannot
 * happen to a caller who reads everything after each call. */
PLANCK_API planck_status planck_submit(planck_engine* engine, const planck_order* order);

/* Cancel a resting order by the order_id from its ACCEPTED report. Answered
 * with a CANCELED or a CANCEL_REJECTED report. Only the session that
 * entered the order may cancel it. Anyone else is told the order is
 * unknown, the same as for an id that never existed. */
PLANCK_API planck_status planck_cancel(planck_engine* engine, uint32_t session, uint64_t order_id);

/* Take the oldest unread report. Returns 1 and fills *out, or returns 0 if
 * there is none. Reports come out in the order things happened. */
PLANCK_API int32_t planck_next_report(planck_engine* engine, planck_report* out);
PLANCK_API int32_t planck_next_market_event(planck_engine* engine, planck_market_event* out);

/* PLANCK_NOT_FOUND if the engine does not trade the symbol. */
PLANCK_API planck_status planck_best(const planck_engine* engine, uint16_t symbol, uint8_t side,
                                     planck_quote* out);
/* Total quantity resting at one price. Zero for a price outside the band. */
PLANCK_API planck_status planck_quantity_at(const planck_engine* engine, uint16_t symbol,
                                            int32_t price, uint64_t* out);
PLANCK_API planck_status planck_get_stats(const planck_engine* engine, planck_stats* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PLANCK_H */
