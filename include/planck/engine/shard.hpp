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
    // Zero turns journaling off. Otherwise a power of two: the shard then
    // puts a JournalEntry on journal() for every command it handles, and
    // something must drain that ring or the shard will stall.
    std::uint32_t journal_ring_capacity = 0;

    // Cancel a session's resting orders when it loses its connection (a
    // SessionClosed or AllSessionsClosed command). Off, those commands do
    // nothing and orders rest until they trade or are cancelled by hand. On,
    // the shard keeps a list of resting orders per session: eight bytes for
    // every order of capacity, and two more memory touches when an order
    // comes to rest or leaves the book. A venue wants this on.
    bool cancel_on_disconnect = false;
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
    // SessionClosed and AllSessionsClosed commands, whether or not this
    // shard acts on them.
    std::uint64_t session_closes = 0;
    // Orders cancelled because their session lost its connection.
    std::uint64_t disconnect_cancels = 0;
    // Times the shard had to wait because a consumer had not kept up.
    std::uint64_t stalls = 0;
};

// One matching thread and everything it owns: the order store, the books for
// its symbols, and its rings.
//
//   gateway --commands--> [ shard ] --reports--> gateway   (per session)
//                                   --market---> feed      (public)
//                                   --journal--> recorder  (optional)
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
    using JournalRing = ring::SpscRing<JournalEntry>;

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
    //
    // A session closing may cancel more orders than the stage holds too, but
    // it never waits: it cancels a stage-full at a time, and if a consumer
    // is behind, poll() returns with the rest still to do and carries on
    // the next time it is called. No other command is taken in between.
    std::size_t poll(std::size_t max_commands = kDefaultPollBatch) noexcept;

    // Call poll() until stop becomes true. Idles as the config says.
    void run(const std::atomic<bool>& stop) noexcept;

    // Carry out one command read back from a journal, stamped with the time
    // it was given the first time. For rebuilding state after a restart,
    // before the shard goes live. Market data is published as usual, so a
    // feed sees the same stream again. Nothing is reported to sessions,
    // which no longer exist, and nothing is journaled a second time.
    //
    // Returns false, without having started on this command, if an earlier
    // one is not finished: its output is still waiting for room in a ring.
    // Let the consumers run and call again. Only the shard's own thread may
    // call this, and never while a gateway is feeding commands().
    [[nodiscard]] bool try_replay(const Command& command, std::uint64_t timestamp_ns) noexcept;

    // Finish the last command as far as the rings allow: carry on with a
    // close that is part way through and publish what is staged. True if
    // nothing is left to do. After the last try_replay(), call this until
    // it is: a close may still be in progress when try_replay() returns.
    [[nodiscard]] bool try_finish() noexcept;

    // The gateway is the only producer on commands() and the only consumer
    // on reports(). The feed is the only consumer on market().
    [[nodiscard]] CommandRing& commands() noexcept { return commands_; }
    [[nodiscard]] ReportRing& reports() noexcept { return reports_; }
    [[nodiscard]] MarketRing& market() noexcept { return market_; }
    // The journal recorder is the only consumer on journal().
    [[nodiscard]] JournalRing& journal() noexcept { return journal_; }
    [[nodiscard]] bool journaling() const noexcept { return journaling_; }
    [[nodiscard]] bool cancels_on_disconnect() const noexcept { return owner_links_ != nullptr; }
    // How many commands this shard has handled, replayed ones included. The
    // next JournalEntry will carry this plus one.
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }

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

    // Where an order sits in its session's list of resting orders. Kept
    // beside the order store, one per slot, so the Order itself stays at
    // half a cache line.
    struct OwnerLink {
        mem::Index next = mem::kNullIndex;
        mem::Index prev = mem::kNullIndex;
    };

    static constexpr std::uint16_t kNoBook = 0xFFFF;
    // One list head for every value a session id can take.
    static constexpr std::size_t kOwners = std::size_t{1} << 16;
    static_assert(sizeof(core::SessionId) == 2);

    [[nodiscard]] core::Status init_memory(const ShardConfig& config) noexcept;
    [[nodiscard]] core::Status init_books(const ShardConfig& config) noexcept;

    [[nodiscard]] book::OrderBook* find_book(core::SymbolId symbol) const noexcept {
        if (symbol.raw() >= core::kMaxSymbols) {
            return nullptr;
        }
        const std::uint16_t index = book_index_[symbol.raw()];
        return (index == kNoBook) ? nullptr : (books_ + index);
    }

    // forced_timestamp_ns of zero means "read the clock".
    void handle(const Command& command, std::uint64_t forced_timestamp_ns) noexcept;
    void handle_new(const Command& command) noexcept;
    void handle_cancel(const Command& command) noexcept;
    // Start cancelling everything one session, or every session, has
    // resting. Does nothing if this shard does not cancel on disconnect.
    void begin_close(std::size_t first_owner, std::size_t owners) noexcept;
    // Cancel as many more as the stage has room for. Ends the close when
    // nothing is left.
    void continue_close() noexcept;
    [[nodiscard]] bool closing() const noexcept { return close_owners_left_ != 0; }

    [[nodiscard]] static constexpr mem::Index slot_of(core::OrderId id) noexcept {
        return static_cast<mem::Index>(id.raw() & book::OrderStore::kSlotMask);
    }
    // Keep the per-session lists in step with the book. Both do nothing if
    // this shard does not cancel on disconnect.
    void link_owner(mem::Index slot, std::uint32_t owner) noexcept {
        if (owner_links_ == nullptr) {
            return;
        }
        PLANCK_ASSERT(slot < store_.capacity() && owner < kOwners);
        const mem::Index head = owner_heads_[owner];
        owner_links_[slot] = OwnerLink{.next = head, .prev = mem::kNullIndex};
        if (head != mem::kNullIndex) {
            owner_links_[head].prev = slot;
        }
        owner_heads_[owner] = slot;
    }
    void unlink_owner(mem::Index slot, std::uint32_t owner) noexcept {
        if (owner_links_ == nullptr) {
            return;
        }
        PLANCK_ASSERT(slot < store_.capacity() && owner < kOwners);
        const OwnerLink link = owner_links_[slot];
        if (link.prev == mem::kNullIndex) {
            PLANCK_ASSERT(owner_heads_[owner] == slot);
            owner_heads_[owner] = link.next;
        } else {
            owner_links_[link.prev].next = link.next;
        }
        if (link.next != mem::kNullIndex) {
            owner_links_[link.next].prev = link.prev;
        }
    }
    void on_trade(const book::Trade& trade) noexcept;
    void emit_accepted(core::OrderId id) noexcept;

    template <wire::Message T>
    void report(core::SessionId session, const T& message) noexcept;
    template <wire::Message T>
    void publish(const T& message) noexcept;

    // Re-anchor the clock to the wall clock if that has not been done for a
    // while. Called only when there is no work waiting.
    void refresh_clock() noexcept;

    // Move staged messages into the rings if there is room. True if nothing
    // is left waiting.
    [[nodiscard]] bool try_flush() noexcept;
    void flush_reports_blocking() noexcept;
    void flush_market_blocking() noexcept;
    void idle() const noexcept;

    // Hot state first.
    book::OrderBook* books_ = nullptr;
    // Both null unless the shard cancels on disconnect.
    OwnerLink* owner_links_ = nullptr;
    mem::Index* owner_heads_ = nullptr;
    Active active_{};
    std::uint64_t match_sequence_ = 1;
    std::uint64_t shard_bits_ = 0;
    // Timestamps handed out never go backwards, even across a re-anchor.
    std::uint64_t last_timestamp_ns_ = 0;
    std::uint64_t sequence_ = 0;
    bool journaling_ = false;
    // True while try_replay() is handling a command.
    bool replaying_ = false;
    // A close in progress: the next order to cancel, the session whose list
    // it is on, and how many sessions are still to go, this one included.
    // Zero sessions to go means no close is in progress.
    mem::Index close_slot_ = mem::kNullIndex;
    std::uint32_t close_owner_ = 0;
    std::uint32_t close_owners_left_ = 0;
    // Whether the close came from try_replay(), which reports to nobody.
    bool close_replayed_ = false;
    // One command makes one journal entry, and the next command is not
    // taken until it has gone out, so a stage of one is enough.
    bool journal_pending_ = false;
    JournalEntry journal_entry_{};
    std::size_t report_count_ = 0;
    std::size_t market_count_ = 0;
    IdlePolicy idle_ = IdlePolicy::Spin;
    sys::TscClock clock_{};
    book::OrderStore store_{};
    ShardStats stats_{};

    CommandRing commands_{};
    ReportRing reports_{};
    MarketRing market_{};
    JournalRing journal_{};

    std::array<std::uint16_t, core::kMaxSymbols> book_index_{};
    std::array<Report, kStageCapacity> report_stage_{};
    std::array<MarketEvent, kStageCapacity> market_stage_{};

    std::size_t book_count_ = 0;
    sys::HotThreadReport thread_{};
    mem::Arena arena_{};
};

}  // namespace planck::engine

#endif  // PLANCK_ENGINE_SHARD_HPP
