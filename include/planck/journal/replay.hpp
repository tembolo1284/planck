#ifndef PLANCK_JOURNAL_REPLAY_HPP
#define PLANCK_JOURNAL_REPLAY_HPP

#include <cstdint>
#include <memory>
#include <type_traits>

#include "planck/core/status.hpp"
#include "planck/engine/shard.hpp"
#include "planck/journal/format.hpp"
#include "planck/journal/reader.hpp"

namespace planck::journal {

struct ReplayResult {
    // Commands replayed into the shard.
    std::uint64_t records = 0;
    // Highest session id any of them carried. A gateway started afterwards
    // must hand out ids above this, or a new client could be mistaken for
    // the owner of an old order.
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
template <typename OnStall>
    requires std::is_nothrow_invocable_v<OnStall&>
[[nodiscard]] core::Status replay(const char* path, engine::Shard& shard,
                                  std::uint64_t fingerprint, ReplayResult& out,
                                  OnStall&& on_stall) noexcept {
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

}  // namespace planck::journal

#endif  // PLANCK_JOURNAL_REPLAY_HPP
