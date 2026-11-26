#include "planck/engine/shard.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include <sched.h>

#include "planck/core/bits.hpp"
#include "planck/core/contract.hpp"
#include "planck/core/hint.hpp"

namespace planck::engine {

namespace {

using core::ok;
using core::Status;

// Room for alignment gaps between the arena's allocations.
constexpr std::size_t kArenaSlack = std::size_t{256} << 10;
// How long a blocking flush may wait for a consumer before the shard gives
// up. A consumer stuck for this long is dead, and running on would only
// build an unbounded backlog.
constexpr std::uint64_t kMaxStallSpins = std::uint64_t{1} << 34;
constexpr core::Nanos kCalibrationWindow{50'000'000};

// wire::RejectCode starts with the same values as book::RejectReason, so the
// conversion is a cast. These keep the two enums from drifting apart.
constexpr bool same_code(book::RejectReason reason, wire::RejectCode code) noexcept {
    return std::to_underlying(reason) == std::to_underlying(code);
}
static_assert(same_code(book::RejectReason::None, wire::RejectCode::None));
static_assert(same_code(book::RejectReason::InvalidQuantity, wire::RejectCode::InvalidQuantity));
static_assert(same_code(book::RejectReason::PriceOutOfBand, wire::RejectCode::PriceOutOfBand));
static_assert(same_code(book::RejectReason::NotFillable, wire::RejectCode::NotFillable));
static_assert(same_code(book::RejectReason::BookFull, wire::RejectCode::BookFull));
static_assert(same_code(book::RejectReason::UnknownOrder, wire::RejectCode::UnknownOrder));
static_assert(book::kRejectReasonCount == 6);

constexpr wire::RejectCode to_wire(book::RejectReason reason) noexcept {
    return static_cast<wire::RejectCode>(std::to_underlying(reason));
}

constexpr bool valid_ring_size(std::uint32_t capacity, std::size_t minimum) noexcept {
    return core::is_pow2(capacity) && capacity >= minimum && capacity <= Shard::kMaxRingCapacity;
}

Status validate(const ShardConfig& config) noexcept {
    if (config.shard.raw() >= core::kMaxShards) {
        return Status::OutOfRange;
    }
    if (config.symbols.empty() || config.symbols.size() > core::kMaxSymbols) {
        return Status::InvalidArgument;
    }
    if (config.order_capacity == 0 || config.order_capacity > book::OrderStore::kMaxCapacity) {
        return Status::InvalidArgument;
    }
    if (!valid_ring_size(config.command_ring_capacity, 2) ||
        !valid_ring_size(config.report_ring_capacity, Shard::kStageCapacity) ||
        !valid_ring_size(config.market_ring_capacity, Shard::kStageCapacity)) {
        return Status::InvalidArgument;
    }
    if (config.journal_ring_capacity != 0 && !valid_ring_size(config.journal_ring_capacity, 2)) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

// Bytes the shard will carve from its arena. Mirrors init_memory and
// init_books: change one and you must change the other.
std::size_t arena_bytes(const ShardConfig& config) noexcept {
    std::size_t total = kArenaSlack;
    total += std::size_t{config.order_capacity} * sizeof(book::Order);
    total += std::size_t{config.command_ring_capacity} * sizeof(Command);
    total += std::size_t{config.report_ring_capacity} * sizeof(Report);
    total += std::size_t{config.market_ring_capacity} * sizeof(MarketEvent);
    total += std::size_t{config.journal_ring_capacity} * sizeof(JournalEntry);
    total += config.symbols.size() * sizeof(book::OrderBook);
    for (const SymbolConfig& symbol : config.symbols) {
        // One Level per tick, plus one bit per tick in each of two bitmaps.
        const std::size_t ticks = std::min(symbol.band_ticks, book::OrderBook::kMaxBandTicks);
        total += ticks * sizeof(book::Level);
        total += 2 * (ticks / 8);
    }
    return total;
}

}  // namespace

// Adapter that lets OrderBook::add call back into the shard.
struct Shard::Sink {
    Shard& shard;
    void on_trade(const book::Trade& trade) noexcept { shard.on_trade(trade); }
};

Status Shard::init(const ShardConfig& config) noexcept {
    if (initialised()) {
        return Status::Duplicate;
    }
    if (const Status status = validate(config); !ok(status)) {
        return status;
    }

    // Pin before touching memory, so first touch happens on the right core.
    if (config.pin) {
        const sys::HotThreadConfig thread{.cpu = config.cpu,
                                          .name = "planck-shard",
                                          .realtime_priority = config.realtime_priority,
                                          .require_isolated = config.require_isolated};
        if (const Status status = sys::configure_hot_thread(thread, thread_); !ok(status)) {
            return status;
        }
    }

    if (const Status status = init_memory(config); !ok(status)) {
        return status;
    }
    if (const Status status = clock_.calibrate(kCalibrationWindow); !ok(status)) {
        return status;
    }

    shard_bits_ = static_cast<std::uint64_t>(config.shard.raw())
                  << (book::OrderStore::kSlotBits + book::OrderStore::kSequenceBits);
    match_sequence_ = 1;
    idle_ = config.idle;

    // Books go last: initialised() keys off books_, so a failure anywhere
    // above leaves the shard reporting "not initialised".
    return init_books(config);
}

Status Shard::init_memory(const ShardConfig& config) noexcept {
    if (const Status status = arena_.init(arena_bytes(config), config.huge_pages); !ok(status)) {
        return status;
    }

    const auto orders = arena_.allocate_array<book::Order>(config.order_capacity);
    const auto commands = arena_.allocate_array<Command>(config.command_ring_capacity);
    const auto reports = arena_.allocate_array<Report>(config.report_ring_capacity);
    const auto market = arena_.allocate_array<MarketEvent>(config.market_ring_capacity);
    if (orders.empty() || commands.empty() || reports.empty() || market.empty()) {
        return Status::Capacity;
    }

    if (const Status status = store_.init(orders, config.shard); !ok(status)) {
        return status;
    }
    if (const Status status = commands_.init(commands); !ok(status)) {
        return status;
    }
    if (const Status status = reports_.init(reports); !ok(status)) {
        return status;
    }
    if (config.journal_ring_capacity != 0) {
        const auto journal = arena_.allocate_array<JournalEntry>(config.journal_ring_capacity);
        if (journal.empty()) {
            return Status::Capacity;
        }
        if (const Status status = journal_.init(journal); !ok(status)) {
            return status;
        }
        journaling_ = true;
    }
    return market_.init(market);
}

Status Shard::init_books(const ShardConfig& config) noexcept {
    PLANCK_EXPECT(!config.symbols.empty() && config.symbols.size() <= core::kMaxSymbols);

    const std::size_t count = config.symbols.size();
    std::byte* const raw =
        arena_.allocate(count * sizeof(book::OrderBook), alignof(book::OrderBook));
    if (raw == nullptr) {
        return Status::Capacity;
    }
    auto* const books = reinterpret_cast<book::OrderBook*>(raw);
    book_index_.fill(kNoBook);

    for (std::size_t i = 0; i < count; ++i) {
        const SymbolConfig& symbol = config.symbols[i];
        if (symbol.symbol.raw() >= core::kMaxSymbols) {
            return Status::OutOfRange;
        }
        if (book_index_[symbol.symbol.raw()] != kNoBook) {
            return Status::Duplicate;
        }
        book::OrderBook* const book = std::construct_at(books + i);
        const book::BookConfig book_config{.symbol = symbol.symbol,
                                           .band_low = symbol.band_low,
                                           .band_ticks = symbol.band_ticks};
        if (const Status status = book->init(book_config, store_, arena_); !ok(status)) {
            return status;
        }
        book_index_[symbol.symbol.raw()] = static_cast<std::uint16_t>(i);
    }

    book_count_ = count;
    books_ = books;
    PLANCK_ENSURE(initialised());
    return Status::Ok;
}

std::size_t Shard::poll(std::size_t max_commands) noexcept {
    PLANCK_EXPECT(initialised());

    std::size_t handled = 0;
    while (handled < max_commands) {
        // Publish the previous command's messages before taking another. If
        // a consumer is behind, stop here and let the backlog show upstream.
        if (!try_flush()) {
            ++stats_.stalls;
            break;
        }
        Command command;
        if (!commands_.try_pop(command)) {
            break;
        }
        handle(command, 0);
        ++handled;
    }
    static_cast<void>(try_flush());
    if (handled == 0) {
        refresh_clock();
    }
    return handled;
}

void Shard::run(const std::atomic<bool>& stop) noexcept {
    PLANCK_EXPECT(initialised());
    // The one loop in the shard with no fixed bound. It ends when asked to.
    while (!stop.load(std::memory_order_relaxed)) {
        if (poll(kDefaultPollBatch) == 0) {
            idle();
        }
    }
}

bool Shard::try_replay(const Command& command, std::uint64_t timestamp_ns) noexcept {
    PLANCK_EXPECT(initialised());
    PLANCK_EXPECT(timestamp_ns != 0);
    if (!try_flush()) {
        ++stats_.stalls;
        return false;
    }
    replaying_ = true;
    handle(command, timestamp_ns);
    replaying_ = false;
    static_cast<void>(try_flush());
    return true;
}

void Shard::handle(const Command& command, std::uint64_t forced_timestamp_ns) noexcept {
    PLANCK_EXPECT(report_count_ == 0 && market_count_ == 0 && !journal_pending_);

    ++stats_.commands;
    ++sequence_;
    // One clock read per command. Every message it causes carries this time.
    // A re-anchor may move the clock back a little, so clamp: timestamps in
    // the order of events must never decrease. A replayed command carries
    // the time it was given the first time round.
    const std::uint64_t now =
        (forced_timestamp_ns != 0) ? forced_timestamp_ns : clock_.now().raw();
    last_timestamp_ns_ = std::max(last_timestamp_ns_, now);
    if (journaling_ && !replaying_) {
        journal_entry_ = JournalEntry{
            .sequence = sequence_, .timestamp_ns = last_timestamp_ns_, .command = command};
        journal_pending_ = true;
    }
    active_ = Active{.ingress_tsc = command.ingress_tsc,
                     .now_ns = last_timestamp_ns_,
                     .client_order_id = command.id,
                     .taker_left = command.qty,
                     .symbol = command.symbol,
                     .session = command.session,
                     .accepted = false};

    if (command.kind == CommandKind::NewOrder) {
        handle_new(command);
    } else {
        PLANCK_ASSERT(command.kind == CommandKind::CancelOrder);
        handle_cancel(command);
    }
}

void Shard::handle_new(const Command& command) noexcept {
    book::OrderBook* const book = find_book(command.symbol);
    book::AddResult result{.reject = book::RejectReason::None};
    wire::RejectCode reject = wire::RejectCode::UnknownSymbol;
    if (book != nullptr) [[likely]] {
        Sink sink{*this};
        result = book->add(book::NewOrder{.side = command.side,
                                          .type = command.type,
                                          .tif = command.tif,
                                          .price = command.price,
                                          .qty = command.qty,
                                          .owner = command.session.raw()},
                           sink);
        reject = to_wire(result.reject);
    }

    if (reject != wire::RejectCode::None) [[unlikely]] {
        PLANCK_ASSERT(!active_.accepted);
        wire::OrderRejected message{};
        message.reason = reject;
        message.client_order_id = command.id;
        report(command.session, message);
        ++stats_.rejected;
        return;
    }

    // If the order traded, on_trade already sent the acceptance ahead of the
    // first fill. Otherwise it goes out now.
    if (!active_.accepted) {
        emit_accepted(result.id);
    }
    ++stats_.accepted;

    if (result.resting > core::Qty{0}) {
        wire::MdAddOrder message{};
        message.symbol_id = command.symbol.raw();
        message.side = std::to_underlying(command.side);
        message.order_id = result.id.raw();
        message.price = command.price.raw();
        message.qty = result.resting.raw();
        message.timestamp = active_.now_ns;
        publish(message);
        return;
    }

    const core::Qty unfilled = command.qty - result.filled;
    if (unfilled > core::Qty{0}) {
        // IOC or market order that ran out of book.
        wire::Canceled message{};
        message.reason = wire::CancelReason::Unfilled;
        message.order_id = result.id.raw();
        message.qty = unfilled.raw();
        message.timestamp = active_.now_ns;
        report(command.session, message);
    }
}

void Shard::handle_cancel(const Command& command) noexcept {
    const core::OrderId id{command.id};
    const mem::Index slot = store_.find(id);
    // An order that belongs to another session is reported exactly like one
    // that does not exist. A session learns nothing about other sessions.
    if (slot == mem::kNullIndex || store_[slot].owner != command.session.raw()) {
        wire::CancelRejected message{};
        message.reason = wire::RejectCode::UnknownOrder;
        message.order_id = command.id;
        report(command.session, message);
        ++stats_.cancel_rejects;
        return;
    }

    const core::SymbolId symbol = store_[slot].symbol;
    book::OrderBook* const book = find_book(symbol);
    PLANCK_ASSERT(book != nullptr);
    const book::CancelResult result = book->cancel(id);
    PLANCK_ASSERT(result.reject == book::RejectReason::None);

    wire::Canceled canceled{};
    canceled.reason = wire::CancelReason::UserRequested;
    canceled.order_id = command.id;
    canceled.qty = result.cancelled.raw();
    canceled.timestamp = active_.now_ns;
    report(command.session, canceled);

    wire::MdDeleted deleted{};
    deleted.symbol_id = symbol.raw();
    deleted.order_id = command.id;
    deleted.timestamp = active_.now_ns;
    publish(deleted);
    ++stats_.cancels;
}

void Shard::on_trade(const book::Trade& trade) noexcept {
    PLANCK_EXPECT(active_.taker_left.raw() >= trade.qty.raw());

    if (!active_.accepted) {
        emit_accepted(trade.aggressor);
    }
    const std::uint64_t match_id = shard_bits_ | match_sequence_;
    ++match_sequence_;
    active_.taker_left -= trade.qty;

    wire::Executed taker{};
    taker.liquidity = wire::Liquidity::Taker;
    taker.order_id = trade.aggressor.raw();
    taker.match_id = match_id;
    taker.price = trade.price.raw();
    taker.qty = trade.qty.raw();
    taker.leaves = active_.taker_left.raw();
    taker.timestamp = active_.now_ns;
    report(active_.session, taker);

    wire::Executed maker = taker;
    maker.liquidity = wire::Liquidity::Maker;
    maker.order_id = trade.resting.raw();
    maker.leaves = trade.resting_leaves.raw();
    report(core::SessionId{static_cast<std::uint16_t>(trade.resting_owner)}, maker);

    wire::MdExecuted print{};
    print.symbol_id = active_.symbol.raw();
    print.order_id = trade.resting.raw();
    print.match_id = match_id;
    print.qty = trade.qty.raw();
    print.price = trade.price.raw();
    print.timestamp = active_.now_ns;
    publish(print);
    ++stats_.trades;
}

void Shard::emit_accepted(core::OrderId id) noexcept {
    PLANCK_EXPECT(!active_.accepted);
    PLANCK_EXPECT(id != core::kNoOrderId);

    wire::OrderAccepted message{};
    message.client_order_id = active_.client_order_id;
    message.order_id = id.raw();
    message.timestamp = active_.now_ns;
    report(active_.session, message);
    active_.accepted = true;
}

template <wire::Message T>
void Shard::report(core::SessionId session, const T& message) noexcept {
    if (replaying_) [[unlikely]] {
        return;  // the session this was for ended with the last run
    }
    if (report_count_ == kStageCapacity) [[unlikely]] {
        flush_reports_blocking();
    }
    Report& slot = report_stage_[report_count_];
    slot.ingress_tsc = active_.ingress_tsc;
    slot.session = session;
    slot.length = sizeof(T);
    std::memcpy(slot.bytes.data(), static_cast<const void*>(&message), sizeof(T));
    ++report_count_;
}

template <wire::Message T>
void Shard::publish(const T& message) noexcept {
    static_assert(sizeof(T) <= kMaxMarketMessageSize);
    if (market_count_ == kStageCapacity) [[unlikely]] {
        flush_market_blocking();
    }
    MarketEvent& slot = market_stage_[market_count_];
    slot.length = sizeof(T);
    std::memcpy(slot.bytes.data(), static_cast<const void*>(&message), sizeof(T));
    ++market_count_;
}

bool Shard::try_flush() noexcept {
    PLANCK_EXPECT(report_count_ <= kStageCapacity && market_count_ <= kStageCapacity);

    if (report_count_ != 0 &&
        reports_.try_push_all(std::span<const Report>{report_stage_.data(), report_count_})) {
        report_count_ = 0;
    }
    if (market_count_ != 0 &&
        market_.try_push_all(std::span<const MarketEvent>{market_stage_.data(), market_count_})) {
        market_count_ = 0;
    }
    if (journal_pending_ && journal_.try_push(journal_entry_)) {
        journal_pending_ = false;
    }
    return report_count_ == 0 && market_count_ == 0 && !journal_pending_;
}

void Shard::flush_reports_blocking() noexcept {
    PLANCK_EXPECT(report_count_ != 0);
    const std::span<const Report> staged{report_stage_.data(), report_count_};
    std::uint64_t spins = 0;
    while (!reports_.try_push_all(staged)) {
        idle();
        ++spins;
        PLANCK_VERIFY(spins < kMaxStallSpins);
    }
    stats_.stalls += (spins != 0) ? 1U : 0U;
    report_count_ = 0;
}

void Shard::flush_market_blocking() noexcept {
    PLANCK_EXPECT(market_count_ != 0);
    const std::span<const MarketEvent> staged{market_stage_.data(), market_count_};
    std::uint64_t spins = 0;
    while (!market_.try_push_all(staged)) {
        idle();
        ++spins;
        PLANCK_VERIFY(spins < kMaxStallSpins);
    }
    stats_.stalls += (spins != 0) ? 1U : 0U;
    market_count_ = 0;
}

void Shard::refresh_clock() noexcept {
    PLANCK_EXPECT(clock_.calibrated());
    // Once a second is plenty. The check itself is one counter read.
    if ((sys::tsc_now() - clock_.anchor_tsc()) > clock_.ticks_per_second()) {
        clock_.reanchor();
    }
}

void Shard::idle() const noexcept {
    if (idle_ == IdlePolicy::Spin) {
        core::cpu_relax();
    } else {
        // Nothing useful to do if the kernel declines to reschedule.
        static_cast<void>(::sched_yield());
    }
}

}  // namespace planck::engine
