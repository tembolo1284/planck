#ifndef PLANCK_JOURNAL_RECORDER_HPP
#define PLANCK_JOURNAL_RECORDER_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "planck/core/config.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/engine/shard.hpp"
#include "planck/journal/writer.hpp"

namespace planck::journal {

struct RecorderConfig {
    // Every shard, already initialised with a journal ring, and already
    // brought up to date by replay() if there was a journal to replay. The
    // recorder becomes the only consumer of each shard's journal ring.
    std::span<engine::Shard* const> shards{};
    // fingerprint() of each shard's configuration, in the same order.
    std::span<const std::uint64_t> fingerprints{};
    // Where the files live. It must exist.
    std::string_view directory{};

    // Longest that records handed to the kernel may wait before the disk is
    // made to catch up. Zero syncs after every write: safest and slowest.
    core::Nanos sync_interval{10'000'000};

    bool pin = false;
    std::size_t cpu = 0;
    engine::IdlePolicy idle = engine::IdlePolicy::Spin;
};

// Plain counters, written only by the recorder thread.
struct RecorderStats {
    std::uint64_t records = 0;
    std::uint64_t bytes = 0;
    std::uint64_t writes = 0;
    std::uint64_t syncs = 0;
    // Bytes of torn tail cut from existing files at startup.
    std::uint64_t truncated_bytes = 0;
    // Records thrown away after the journal failed.
    std::uint64_t discarded = 0;
};

// Writes every shard's journal to disk, on a thread of its own.
//
//   shard --journal ring--> [ recorder ] --> shard-NN.journal
//
// The shard never waits for the disk. It puts each command it has handled
// on a ring and carries on. So a command is answered before it is durable:
// if the machine loses power, the last few milliseconds of commands are
// gone even though clients were told about them. sync_interval bounds how
// many.
//
// If the disk fails or fills, the recorder stops writing, sets failed(),
// and keeps emptying the rings so that matching carries on. From that
// moment the journal no longer describes the venue, and a restart would
// lose everything after it. Whoever runs the venue must watch failed().
class Recorder {
public:
    static constexpr std::size_t kMaxRecordsPerPoll = 4096;

    Recorder() noexcept = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Open, or create, one journal per shard. Protocol if a journal's next
    // sequence number is not the one its shard will produce, which means the
    // shard was not replayed from that journal.
    [[nodiscard]] core::Status init(const RecorderConfig& config) noexcept;

    // One pass: move what the shards have produced into the files, and sync
    // if it is time. Returns how many records it took.
    std::size_t poll() noexcept;

    // Call poll() until stop becomes true, then write and sync what is left.
    void run(const std::atomic<bool>& stop) noexcept;

    // Write out and sync everything taken so far. For shutdown.
    void finish() noexcept;

    [[nodiscard]] bool initialised() const noexcept { return stream_count_ != 0; }
    // True once a write or sync has failed. It never goes back to false.
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    // What that failure was.
    [[nodiscard]] core::Status error() const noexcept { return error_; }
    [[nodiscard]] const RecorderStats& stats() const noexcept { return stats_; }

private:
    struct Stream {
        engine::Shard* shard = nullptr;
        Writer writer{};
        // Handed to the kernel but not yet known to be on the disk.
        bool unsynced = false;
    };

    void fail(core::Status status) noexcept;
    void sync_all() noexcept;
    void idle() const noexcept;

    std::array<Stream, core::kMaxShards> streams_{};
    std::size_t stream_count_ = 0;
    std::uint64_t sync_interval_ns_ = 0;
    std::uint64_t last_sync_ns_ = 0;
    bool failed_ = false;
    core::Status error_ = core::Status::Ok;
    engine::IdlePolicy idle_ = engine::IdlePolicy::Spin;
    RecorderStats stats_{};
};

}  // namespace planck::journal

#endif  // PLANCK_JOURNAL_RECORDER_HPP
