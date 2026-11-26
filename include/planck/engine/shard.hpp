#ifndef PLANCK_ENGINE_SHARD_HPP
#define PLANCK_ENGINE_SHARD_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "planck/book/order_book.hpp"
#include "planck/book/order_store.hpp"
#include "planck/core/config.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/engine/messages.hpp"
#include "planck/mem/arena.hpp"
#include "planck/ring/spsc.hpp"
#include "planck/sys/clock.hpp"
#include "planck/sys/cpu.hpp"
#include "planck/sys/memory.hpp"
#include "planck/wire/codec.hpp"

namespace planck::engine {

// What a shard does when it has nothing to do.
enum class IdlePolicy : std::uint8_t {
    Spin,   // burn the core. Lowest latency. For a dedicated, isolated core.
    Yield,  // give the core away. For laptops and shared machines.
};

struct SymbolConfig {
    core::SymbolId symbol{};
    core::Price band_low{};
    std::uint32_t band_ticks = 0;
};

struct ShardConfig {
    core::ShardId shard{};
    // Symbols this shard trades. Read during init() only.
    std::span<const SymbolConfig> symbols{};

    // Most orders that can rest at once across all of this shard's books.
    std::uint32_t order_capacity = 1U << 20;
    // Ring sizes. Powers of two. The report and market rings must hold at
    // least Shard::kStageCapacity entries.
    std::uint32_t command_ring_capacity = 1U << 16;
    std::uint32_t report_ring_capacity = 1U << 16;
    std::uint32_t market_ring_capacity = 1U << 16;
    sys::HugePages huge_pages = sys::HugePages::Try;

    // Thread placement. With pin off, the other three are ignored.
    bool pin = true;
    std::size_t cpu = 0;
    bool require_isolated = false;
    int realtime_priority = 0;

    IdlePolicy idle = IdlePolicy::Spin;
};

// Plain counters, written only by the shard thread. Reading them from another
// thread while the shard runs gives approximate values.
struct ShardStats {
    std::uint64_t commands = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t trades = 0;
    std::uint64_t cancels = 0;
    std::uint64_t cancel_rejects = 0;
    // Times the shard had to wait because a consumer had not kept up.
    std::uint64_t stalls = 0;
};

// One matching thread and everything it owns: the order store, the books for
// its symbols, and three rings.
//
//   gateway --commands--> [ shard ] --reports--> gateway   (per session)
//                                   --market---> feed      (public)
//
// No lock and no atomic sits between a command and its book. Other threads
// touch a shard only through the rings.
//
// Lifecycle: construct anywhere. Call init() from the thread that will run
// the shard, so the memory it touches lands on that thread's NUMA node. Then
// call run(), or call poll() yourself.
class alignas(core::kCacheLine) Shard {
public:
    using CommandRing = ring::SpscRing<Command>;
    using ReportRing = ring::SpscRing<Report>;
    using MarketRing = ring::SpscRing<MarketEvent>;

    // Messages from one command are collected here and published together.
    static constexpr std::size_t kStageCapacity = 128;
    static constexpr std::size_t kDefaultPollBatch = 64;
    static constexpr std::uint32_t kMaxRingCapacity = 1U << 24;

    Shard() noexcept = default;
    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;

    // Pin the calling thread if asked to, map memory, build the books and
    // calibrate the clock. Blocks for about 50 ms on calibration.
    [[nodiscard]] core::Status init(const ShardConfig& config) noexcept;

    // Handle up to max_commands queued commands and publish what they
    // produce. Returns how many were handled. Never blocks, with one
    // exception: a single order that produces more messages than fit in the
    // stage while a consumer's ring is full has to wait for room, because
    // fills already reported cannot be taken back.
    //
    // If a consumer has fallen behind, poll() stops taking commands until it
    // catches up. The command ring then fills and the gateway sees
    // backpressure. No message is ever dropped.
    std::size_t poll(std::size_t max_commands = kDefaultPollBatch) noexcept;

    // Call poll() until stop becomes true. Idles as the config says.
    void run(const std::atomic<bool>& stop) noexcept;

    // The gateway is the only producer on commands() and the only consumer
    // on reports(). The feed is the only consumer on market().
    [[nodiscard]] CommandRing& commands() noexcept { return commands_; }
    [[nodiscard]] ReportRing& reports() noexcept { return reports_; }
    [[nodiscard]] MarketRing& market() noexcept { return market_; }

    // A symbol's book, or nullptr if this shard does not trade it. Only the
    // shard thread may read a book while the shard is running.
    [[nodiscard]] const book::OrderBook* book(core::SymbolId symbol) const noexcept {
        return find_book(symbol);
    }

    [[nodiscard]] bool initialised() const noexcept { return books_ != nullptr; }
    [[nodiscard]] core::ShardId id() const noexcept { return store_.shard(); }
    [[nodiscard]] const ShardStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const sys::HotThreadReport& thread_report() const noexcept { return thread_; }
    [[nodiscard]] const book::OrderStore& store() const noexcept { return store_; }

private:
    struct Sink;

    // The command being handled. Trade callbacks read it.
    struct Active {
        std::uint64_t ingress_tsc = 0;
        std::uint64_t now_ns = 0;
        std::uint64_t client_order_id = 0;
        core::Qty taker_left{};
        core::SymbolId symbol{};
        core::SessionId session{};
        bool accepted = false;
    };

    static constexpr std::uint16_t kNoBook = 0xFFFF;

    [[nodiscard]] core::Status init_memory(const ShardConfig& config) noexcept;
    [[nodiscard]] core::Status init_books(const ShardConfig& config) noexcept;

    [[nodiscard]] book::OrderBook* find_book(core::SymbolId symbol) const noexcept {
        if (symbol.raw() >= core::kMaxSymbols) {
            return nullptr;
        }
        const std::uint16_t index = book_index_[symbol.raw()];
        return (index == kNoBook) ? nullptr : (books_ + index);
    }

    void handle(const Command& command) noexcept;
    void handle_new(const Command& command) noexcept;
    void handle_cancel(const Command& command) noexcept;
    void on_trade(const book::Trade& trade) noexcept;
    void emit_accepted(core::OrderId id) noexcept;

    template <wire::Message T>
    void report(core::SessionId session, const T& message) noexcept;
    template <wire::Message T>
    void publish(const T& message) noexcept;

    // Move staged messages into the rings if there is room. True if both
    // stages are now empty.
    [[nodiscard]] bool try_flush() noexcept;
    void flush_reports_blocking() noexcept;
    void flush_market_blocking() noexcept;
    void idle() const noexcept;

    // Hot state first.
    book::OrderBook* books_ = nullptr;
    Active active_{};
    std::uint64_t match_sequence_ = 1;
    std::uint64_t shard_bits_ = 0;
    std::size_t report_count_ = 0;
    std::size_t market_count_ = 0;
    IdlePolicy idle_ = IdlePolicy::Spin;
    sys::TscClock clock_{};
    book::OrderStore store_{};
    ShardStats stats_{};

    CommandRing commands_{};
    ReportRing reports_{};
    MarketRing market_{};

    std::array<std::uint16_t, core::kMaxSymbols> book_index_{};
    std::array<Report, kStageCapacity> report_stage_{};
    std::array<MarketEvent, kStageCapacity> market_stage_{};

    std::size_t book_count_ = 0;
    sys::HotThreadReport thread_{};
    mem::Arena arena_{};
};

}  // namespace planck::engine

#endif  // PLANCK_ENGINE_SHARD_HPP
