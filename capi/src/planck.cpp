#include "planck.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include "planck/book/order_book.hpp"
#include "planck/book/order_store.hpp"
#include "planck/core/config.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/engine/shard.hpp"
#include "planck/wire/codec.hpp"

// The C face of the engine. This file is the only place where the outside
// world's types meet planck's own. Everything that comes in is checked
// here, because a C caller can pass anything. Nothing past this file checks
// again.

namespace {

namespace book = planck::book;
namespace core = planck::core;
namespace engine = planck::engine;
namespace sys = planck::sys;
namespace wire = planck::wire;

// ---- the header and the engine must agree on every number ----

#define PLANCK_SAME(c_value, cpp_value) \
    static_assert(static_cast<std::uint64_t>(c_value) == static_cast<std::uint64_t>(cpp_value))

PLANCK_SAME(PLANCK_OK, core::Status::Ok);
PLANCK_SAME(PLANCK_INVALID_ARGUMENT, core::Status::InvalidArgument);
PLANCK_SAME(PLANCK_OUT_OF_RANGE, core::Status::OutOfRange);
PLANCK_SAME(PLANCK_CAPACITY, core::Status::Capacity);
PLANCK_SAME(PLANCK_NOT_FOUND, core::Status::NotFound);
PLANCK_SAME(PLANCK_DUPLICATE, core::Status::Duplicate);
PLANCK_SAME(PLANCK_WOULD_BLOCK, core::Status::WouldBlock);
PLANCK_SAME(PLANCK_CLOSED, core::Status::Closed);
PLANCK_SAME(PLANCK_IO, core::Status::Io);
PLANCK_SAME(PLANCK_PROTOCOL, core::Status::Protocol);
PLANCK_SAME(PLANCK_UNSUPPORTED, core::Status::Unsupported);
PLANCK_SAME(PLANCK_PERMISSION, core::Status::Permission);
PLANCK_SAME(PLANCK_PERMISSION + 1, core::kStatusCount);

PLANCK_SAME(PLANCK_BUY, core::Side::Buy);
PLANCK_SAME(PLANCK_SELL, core::Side::Sell);
PLANCK_SAME(PLANCK_LIMIT, core::OrderType::Limit);
PLANCK_SAME(PLANCK_MARKET, core::OrderType::Market);
PLANCK_SAME(PLANCK_DAY, core::TimeInForce::Day);
PLANCK_SAME(PLANCK_IOC, core::TimeInForce::ImmediateOrCancel);
PLANCK_SAME(PLANCK_FOK, core::TimeInForce::FillOrKill);

PLANCK_SAME(PLANCK_REJECT_NONE, wire::RejectCode::None);
PLANCK_SAME(PLANCK_REJECT_INVALID_QUANTITY, wire::RejectCode::InvalidQuantity);
PLANCK_SAME(PLANCK_REJECT_PRICE_OUT_OF_BAND, wire::RejectCode::PriceOutOfBand);
PLANCK_SAME(PLANCK_REJECT_NOT_FILLABLE, wire::RejectCode::NotFillable);
PLANCK_SAME(PLANCK_REJECT_BOOK_FULL, wire::RejectCode::BookFull);
PLANCK_SAME(PLANCK_REJECT_UNKNOWN_ORDER, wire::RejectCode::UnknownOrder);
PLANCK_SAME(PLANCK_REJECT_UNKNOWN_SYMBOL, wire::RejectCode::UnknownSymbol);
PLANCK_SAME(PLANCK_CANCEL_REQUESTED, wire::CancelReason::UserRequested);
PLANCK_SAME(PLANCK_CANCEL_UNFILLED, wire::CancelReason::Unfilled);
PLANCK_SAME(PLANCK_MAKER, wire::Liquidity::Maker);
PLANCK_SAME(PLANCK_TAKER, wire::Liquidity::Taker);

PLANCK_SAME(PLANCK_HUGE_PAGES_OFF, sys::HugePages::Off);
PLANCK_SAME(PLANCK_HUGE_PAGES_TRY, sys::HugePages::Try);
PLANCK_SAME(PLANCK_HUGE_PAGES_REQUIRE, sys::HugePages::Require);

PLANCK_SAME(PLANCK_MAX_SYMBOL_ID, core::kMaxSymbols - 1);
PLANCK_SAME(PLANCK_MAX_SHARD, core::kMaxShards - 1);
PLANCK_SAME(PLANCK_MIN_BAND_TICKS, book::OrderBook::kMinBandTicks);

#undef PLANCK_SAME

// The layouts are the interface. A change to any of these is a change of
// PLANCK_ABI_VERSION.
static_assert(sizeof(planck_symbol) == 12 && alignof(planck_symbol) == 4);
static_assert(sizeof(planck_order) == 32 && offsetof(planck_order, session) == 8);
static_assert(offsetof(planck_order, symbol) == 12 && offsetof(planck_order, side) == 14);
static_assert(offsetof(planck_order, price) == 16 && offsetof(planck_order, tif) == 24);
static_assert(sizeof(planck_report) == 64 && offsetof(planck_report, client_order_id) == 8);
static_assert(offsetof(planck_report, timestamp_ns) == 32 && offsetof(planck_report, price) == 40);
static_assert(offsetof(planck_report, liquidity) == 56);
static_assert(sizeof(planck_market_event) == 40 && offsetof(planck_market_event, order_id) == 8);
static_assert(offsetof(planck_market_event, price) == 32);
static_assert(sizeof(planck_quote) == 16 && sizeof(planck_stats) == 56);
static_assert(std::has_unique_object_representations_v<planck_order>);
static_assert(std::has_unique_object_representations_v<planck_report>);
static_assert(std::has_unique_object_representations_v<planck_market_event>);

// ---- sizing ----

// The engine runs on the caller's thread, so nothing drains its outgoing
// rings while it works on a command. They must therefore hold everything
// one command can produce. The worst command is an order that takes out a
// full book. Privately that is an acceptance, two fills for every resting
// order, and a cancel for what is left. Publicly it is one trade print for
// every resting order. An order that rests its remainder cannot do worse:
// it needs a slot of its own before it may match, so it meets at most
// orders - 1 resting orders, and its one add brings the count back to
// `orders`. On top of the worst case comes some slack, so a caller need
// not read after every single call.
constexpr std::uint32_t kSlack = 1024;

constexpr std::size_t worst_reports(std::uint32_t orders) noexcept {
    return (2 * std::size_t{orders}) + 2;
}
constexpr std::size_t worst_market(std::uint32_t orders) noexcept {
    return std::size_t{orders};
}

static_assert(std::bit_ceil(worst_reports(PLANCK_MAX_ORDER_CAPACITY) + kSlack) <=
              engine::Shard::kMaxRingCapacity);
static_assert(PLANCK_MAX_ORDER_CAPACITY <= book::OrderStore::kMaxCapacity);

constexpr planck_status to_c(core::Status status) noexcept {
    return static_cast<planck_status>(std::to_underlying(status));
}

}  // namespace

struct planck_engine {
    engine::Shard shard;
    // Free entries each ring must have before another command is taken.
    std::size_t reports_needed = 0;
    std::size_t market_needed = 0;
};

namespace {

[[nodiscard]] bool has_room(planck_engine& self) noexcept {
    engine::Shard& shard = self.shard;
    const std::size_t reports_free = shard.reports().capacity() - shard.reports().size_approx();
    const std::size_t market_free = shard.market().capacity() - shard.market().size_approx();
    return reports_free >= self.reports_needed && market_free >= self.market_needed;
}

// Hand one command to the shard and have it matched before returning.
[[nodiscard]] planck_status execute(planck_engine& self, const engine::Command& command) noexcept {
    if (!has_room(self) || !self.shard.commands().try_push(command)) {
        return PLANCK_WOULD_BLOCK;
    }
    // There is room for anything this command can produce, so the shard
    // cannot stall, and the command ring held nothing else.
    const std::size_t handled = self.shard.poll(1);
    return (handled == 1) ? PLANCK_OK : PLANCK_WOULD_BLOCK;
}

[[nodiscard]] planck_status check(const planck_config& config) noexcept {
    if (config.struct_size != sizeof(planck_config)) {
        return PLANCK_UNSUPPORTED;
    }
    if (config.symbols == nullptr || config.symbol_count == 0 || config.reserved != 0 ||
        config.huge_pages > PLANCK_HUGE_PAGES_REQUIRE) {
        return PLANCK_INVALID_ARGUMENT;
    }
    if (config.symbol_count > core::kMaxSymbols || config.shard > PLANCK_MAX_SHARD ||
        config.order_capacity > PLANCK_MAX_ORDER_CAPACITY) {
        return PLANCK_OUT_OF_RANGE;
    }
    return PLANCK_OK;
}

void decode_report(const engine::Report& in, planck_report& out) noexcept {
    out = planck_report{};
    out.session = in.session.raw();
    const std::span<const std::byte> bytes{in.bytes.data(), in.length};
    // The shard encoded these itself, so the type byte can be trusted.
    switch (wire::next_frame(bytes).type) {
        case wire::MsgType::OrderAccepted: {
            wire::OrderAccepted message;
            static_cast<void>(wire::decode(bytes, message));
            out.kind = PLANCK_REPORT_ACCEPTED;
            out.client_order_id = message.client_order_id;
            out.order_id = message.order_id;
            out.timestamp_ns = message.timestamp;
            break;
        }
        case wire::MsgType::OrderRejected: {
            wire::OrderRejected message;
            static_cast<void>(wire::decode(bytes, message));
            out.kind = PLANCK_REPORT_REJECTED;
            out.client_order_id = message.client_order_id;
            out.reason = std::to_underlying(message.reason);
            break;
        }
        case wire::MsgType::Executed: {
            wire::Executed message;
            static_cast<void>(wire::decode(bytes, message));
            out.kind = PLANCK_REPORT_EXECUTED;
            out.order_id = message.order_id;
            out.match_id = message.match_id;
            out.timestamp_ns = message.timestamp;
            out.price = message.price;
            out.qty = message.qty;
            out.leaves = message.leaves;
            out.liquidity = std::to_underlying(message.liquidity);
            break;
        }
        case wire::MsgType::Canceled: {
            wire::Canceled message;
            static_cast<void>(wire::decode(bytes, message));
            out.kind = PLANCK_REPORT_CANCELED;
            out.order_id = message.order_id;
            out.timestamp_ns = message.timestamp;
            out.qty = message.qty;
            out.reason = std::to_underlying(message.reason);
            break;
        }
        case wire::MsgType::CancelRejected: {
            wire::CancelRejected message;
            static_cast<void>(wire::decode(bytes, message));
            out.kind = PLANCK_REPORT_CANCEL_REJECTED;
            out.order_id = message.order_id;
            out.reason = std::to_underlying(message.reason);
            break;
        }
        default:
            // Not a message a shard sends. kind stays zero.
            break;
    }
}

void decode_market(const engine::MarketEvent& in, planck_market_event& out) noexcept {
    out = planck_market_event{};
    const std::span<const std::byte> bytes{in.bytes.data(), in.length};
    switch (wire::next_frame(bytes).type) {
        case wire::MsgType::MdAddOrder: {
            wire::MdAddOrder message;
            static_cast<void>(wire::decode(bytes, message));
            out.kind = PLANCK_MARKET_ADD;
            out.symbol = message.symbol_id;
            out.side = message.side;
            out.order_id = message.order_id;
            out.timestamp_ns = message.timestamp;
            out.price = message.price;
            out.qty = message.qty;
            break;
        }
        case wire::MsgType::MdExecuted: {
            wire::MdExecuted message;
            static_cast<void>(wire::decode(bytes, message));
            out.kind = PLANCK_MARKET_TRADE;
            out.symbol = message.symbol_id;
            out.order_id = message.order_id;
            out.match_id = message.match_id;
            out.timestamp_ns = message.timestamp;
            out.price = message.price;
            out.qty = message.qty;
            break;
        }
        case wire::MsgType::MdDeleted: {
            wire::MdDeleted message;
            static_cast<void>(wire::decode(bytes, message));
            out.kind = PLANCK_MARKET_DELETE;
            out.symbol = message.symbol_id;
            out.order_id = message.order_id;
            out.timestamp_ns = message.timestamp;
            break;
        }
        default:
            break;
    }
}

}  // namespace

extern "C" {

uint32_t planck_abi_version(void) {
    return PLANCK_ABI_VERSION;
}

const char* planck_version(void) {
    return PLANCK_VERSION_STRING;
}

const char* planck_status_name(int32_t status) {
    if (status < 0 || status >= static_cast<std::int32_t>(core::kStatusCount)) {
        return "Unknown";
    }
    // The strings are literals, so data() is terminated.
    return core::to_string(static_cast<core::Status>(status)).data();
}

const char* planck_reject_name(uint32_t reason) {
    if (reason >= wire::kRejectCodeCount) {
        return "Unknown";
    }
    return wire::to_string(static_cast<wire::RejectCode>(reason)).data();
}

void planck_config_init(planck_config* config) {
    if (config == nullptr) {
        return;
    }
    *config = planck_config{};
    config->struct_size = sizeof(planck_config);
    config->order_capacity = PLANCK_DEFAULT_ORDER_CAPACITY;
    config->huge_pages = PLANCK_HUGE_PAGES_TRY;
}

planck_status planck_engine_create(const planck_config* config, planck_engine** out) {
    if (out == nullptr) {
        return PLANCK_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (config == nullptr) {
        return PLANCK_INVALID_ARGUMENT;
    }
    if (const planck_status status = check(*config); status != PLANCK_OK) {
        return status;
    }

    // The shard reads its symbols once, during init().
    std::array<engine::SymbolConfig, core::kMaxSymbols> symbols{};
    for (std::uint32_t i = 0; i < config->symbol_count; ++i) {
        const planck_symbol& symbol = config->symbols[i];
        if (symbol.reserved != 0) {
            return PLANCK_INVALID_ARGUMENT;
        }
        symbols[i] = {core::SymbolId{symbol.id}, core::Price{symbol.band_low}, symbol.band_ticks};
    }

    constexpr std::uint32_t default_orders = PLANCK_DEFAULT_ORDER_CAPACITY;
    const std::uint32_t orders =
        (config->order_capacity != 0) ? config->order_capacity : default_orders;
    engine::ShardConfig shard;
    shard.shard = core::ShardId{static_cast<std::uint8_t>(config->shard)};
    shard.symbols = {symbols.data(), config->symbol_count};
    shard.order_capacity = orders;
    shard.command_ring_capacity = 64;
    shard.report_ring_capacity =
        static_cast<std::uint32_t>(std::bit_ceil(worst_reports(orders) + kSlack));
    shard.market_ring_capacity =
        static_cast<std::uint32_t>(std::bit_ceil(worst_market(orders) + kSlack));
    shard.huge_pages = static_cast<sys::HugePages>(config->huge_pages);
    // The caller's thread is the caller's business.
    shard.pin = false;

    // malloc and free, not new and delete, so that a C program using this
    // library does not have to load the C++ runtime as well.
    static_assert(sizeof(planck_engine) % alignof(planck_engine) == 0);
    void* const memory = std::aligned_alloc(alignof(planck_engine), sizeof(planck_engine));
    if (memory == nullptr) {
        return PLANCK_CAPACITY;
    }
    planck_engine* const self = new (memory) planck_engine;
    if (const core::Status status = self->shard.init(shard); !core::ok(status)) {
        planck_engine_destroy(self);
        return to_c(status);
    }
    self->reports_needed = worst_reports(orders);
    self->market_needed = worst_market(orders);
    *out = self;
    return PLANCK_OK;
}

void planck_engine_destroy(planck_engine* engine) {
    if (engine == nullptr) {
        return;
    }
    engine->~planck_engine();
    std::free(engine);
}

planck_status planck_submit(planck_engine* engine, const planck_order* order) {
    if (engine == nullptr || order == nullptr) {
        return PLANCK_INVALID_ARGUMENT;
    }
    if (order->session == 0 || order->session > PLANCK_MAX_SESSION) {
        return PLANCK_INVALID_ARGUMENT;
    }
    for (const std::uint8_t byte : order->reserved) {
        if (byte != 0) {
            return PLANCK_INVALID_ARGUMENT;
        }
    }
    wire::NewOrder message{};
    message.symbol_id = order->symbol;
    message.side = order->side;
    message.order_type = order->type;
    message.client_order_id = order->client_order_id;
    message.price = order->price;
    message.qty = order->qty;
    message.tif = order->tif;
    // The same check the venue's gateway applies to bytes off a socket.
    if (!wire::well_formed(message)) {
        return PLANCK_INVALID_ARGUMENT;
    }
    const core::SessionId session{static_cast<std::uint16_t>(order->session)};
    return execute(*engine, engine::make_command(message, session, 0));
}

planck_status planck_cancel(planck_engine* engine, uint32_t session, uint64_t order_id) {
    if (engine == nullptr || session == 0 || session > PLANCK_MAX_SESSION) {
        return PLANCK_INVALID_ARGUMENT;
    }
    wire::CancelOrder message{};
    message.order_id = order_id;
    const core::SessionId owner{static_cast<std::uint16_t>(session)};
    return execute(*engine, engine::make_command(message, owner, 0));
}

int32_t planck_next_report(planck_engine* engine, planck_report* out) {
    if (engine == nullptr || out == nullptr) {
        return 0;
    }
    engine::Report report;
    if (!engine->shard.reports().try_pop(report)) {
        // A quiet moment. Polling with nothing queued lets the shard line
        // its clock up with the wall clock again if that is due.
        static_cast<void>(engine->shard.poll(0));
        return 0;
    }
    decode_report(report, *out);
    return 1;
}

int32_t planck_next_market_event(planck_engine* engine, planck_market_event* out) {
    if (engine == nullptr || out == nullptr) {
        return 0;
    }
    engine::MarketEvent event;
    if (!engine->shard.market().try_pop(event)) {
        return 0;
    }
    decode_market(event, *out);
    return 1;
}

planck_status planck_best(const planck_engine* engine, uint16_t symbol, uint8_t side,
                          planck_quote* out) {
    if (engine == nullptr || out == nullptr || side > PLANCK_SELL) {
        return PLANCK_INVALID_ARGUMENT;
    }
    *out = planck_quote{};
    const book::OrderBook* const order_book = engine->shard.book(core::SymbolId{symbol});
    if (order_book == nullptr) {
        return PLANCK_NOT_FOUND;
    }
    const book::Quote quote = order_book->best(static_cast<core::Side>(side));
    out->price = quote.valid ? quote.price.raw() : 0;
    out->qty = quote.valid ? quote.qty : 0;
    out->valid = quote.valid ? 1 : 0;
    return PLANCK_OK;
}

planck_status planck_quantity_at(const planck_engine* engine, uint16_t symbol, int32_t price,
                                 uint64_t* out) {
    if (engine == nullptr || out == nullptr) {
        return PLANCK_INVALID_ARGUMENT;
    }
    *out = 0;
    const book::OrderBook* const order_book = engine->shard.book(core::SymbolId{symbol});
    if (order_book == nullptr) {
        return PLANCK_NOT_FOUND;
    }
    *out = order_book->quantity_at(core::Price{price});
    return PLANCK_OK;
}

planck_status planck_get_stats(const planck_engine* engine, planck_stats* out) {
    if (engine == nullptr || out == nullptr) {
        return PLANCK_INVALID_ARGUMENT;
    }
    const engine::ShardStats& stats = engine->shard.stats();
    *out = planck_stats{.commands = stats.commands,
                        .accepted = stats.accepted,
                        .rejected = stats.rejected,
                        .trades = stats.trades,
                        .cancels = stats.cancels,
                        .cancel_rejects = stats.cancel_rejects,
                        .resting = engine->shard.store().in_use()};
    return PLANCK_OK;
}

}  // extern "C"
