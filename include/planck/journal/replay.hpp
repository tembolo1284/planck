#ifndef PLANCK_JOURNAL_REPLAY_HPP
#define PLANCK_JOURNAL_REPLAY_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

#include "planck/core/status.hpp"
#include "planck/engine/shard.hpp"
#include "planck/journal/format.hpp"
#include "planck/journal/reader.hpp"

namespace planck::journal {

struct ReplayResult {
    // Commands replayed into the shard.
    std::uint64_t records = 0;
    // Highest session id any of them carried.
    std::uint16_t max_session = 0;
    // The file ended in a record that never fully reached the disk. It was
    // ignored. That is the normal mark of a crash.
    bool torn_tail = false;
    // Bytes at the end of the file that were not replayed.
    std::uint64_t ignored_bytes = 0;
};

// Rebuild a shard from its journal. The shard must be freshly initialised
// and must not have handled anything yet. Call this on the shard's own
// thread, before any gateway feeds it.
//
// The shard publishes market data for every replayed command, exactly as it
// did the first time. Something must be draining shard.market(), or the
// replay stops when that ring fills. on_stall() is called each time that
// happens: use it to drain the ring, or to wait for the thread that does.
//
//   Ok               done, or there was no journal to replay.
//   InvalidArgument  the journal is for another shard or another
//                    configuration, or the shard is not fresh.
//   Protocol         the journal is damaged somewhere other than its tail.
//   Unsupported      the journal is from another version of the format.
//   WouldBlock       gave up waiting for room in the market ring.
//
// on_command() is shown every command once it has been replayed, in order.
// A venue uses it to learn what the journal says about each session: see
// note_order_id().
template <typename OnStall, typename OnCommand>
    requires std::is_nothrow_invocable_v<OnStall&> &&
             std::is_nothrow_invocable_v<OnCommand&, const engine::Command&>
[[nodiscard]] core::Status replay(const char* path, engine::Shard& shard,
                                  std::uint64_t fingerprint, ReplayResult& out,
                                  OnStall&& on_stall, OnCommand&& on_command) noexcept {
    constexpr std::uint64_t kMaxStalls = std::uint64_t{1} << 32;
    using core::Status;

    out = ReplayResult{};
    if (!shard.initialised() || shard.sequence() != 0) {
        return Status::InvalidArgument;
    }
    // Heap, once, at startup: the reader's buffer is too big for the stack
    // of a thread that may have been given a small one.
    const std::unique_ptr<Reader> reader{new Reader};
    const Status opened = reader->open(path);
    if (opened == Status::NotFound) {
        return Status::Ok;  // first run: nothing to rebuild
    }
    if (!core::ok(opened)) {
        return opened;
    }
    if (reader->header().shard != shard.id().raw() ||
        reader->header().fingerprint != fingerprint) {
        return Status::InvalidArgument;
    }

    engine::JournalEntry entry;
    Status status = reader->next(entry);
    std::uint64_t stalls = 0;
    // One record per pass, so the file's length bounds this.
    while (core::ok(status)) {
        while (!shard.try_replay(entry.command, entry.timestamp_ns)) {
            on_stall();
            ++stalls;
            if (stalls >= kMaxStalls) {
                return Status::WouldBlock;
            }
        }
        on_command(entry.command);
        ++out.records;
        out.max_session = (entry.command.session.raw() > out.max_session)
                              ? entry.command.session.raw()
                              : out.max_session;
        status = reader->next(entry);
    }

    // The last command may have been a close with more orders to cancel
    // than the rings would take in one go. See it through.
    while (!shard.try_finish()) {
        on_stall();
        ++stalls;
        if (stalls >= kMaxStalls) {
            return Status::WouldBlock;
        }
    }

    out.ignored_bytes = reader->file_bytes() - reader->valid_bytes();
    if (status == Status::Protocol) {
        if (reader->records_after_damage()) {
            return Status::Protocol;
        }
        out.torn_tail = true;
        return Status::Ok;
    }
    return (status == Status::Closed) ? Status::Ok : status;
}

// The same, for a caller with no interest in the commands themselves.
template <typename OnStall>
    requires std::is_nothrow_invocable_v<OnStall&>
[[nodiscard]] core::Status replay(const char* path, engine::Shard& shard,
                                  std::uint64_t fingerprint, ReplayResult& out,
                                  OnStall&& on_stall) noexcept {
    return replay(path, shard, fingerprint, out, on_stall,
                  [](const engine::Command&) noexcept {});
}

// For on_command: keep, by session id, the highest client order id that any
// replayed order carried. `highest` has one entry for every session id. A
// gateway given the result knows an order sent again after a restart for
// the repeat it is: see gateway::GatewayConfig::last_order_ids.
inline void note_order_id(const engine::Command& command,
                          std::span<std::uint64_t> highest) noexcept {
    if (command.kind != engine::CommandKind::NewOrder) {
        return;
    }
    const std::size_t session = command.session.raw();
    if (session < highest.size() && command.id > highest[session]) {
        highest[session] = command.id;
    }
}

}  // namespace planck::journal

#endif  // PLANCK_JOURNAL_REPLAY_HPP
