#include "planck/journal/recorder.hpp"

#include <sched.h>

#include "planck/core/contract.hpp"
#include "planck/core/hint.hpp"
#include "planck/sys/clock.hpp"
#include "planck/sys/cpu.hpp"

namespace planck::journal {

using core::ok;
using core::Status;

Status Recorder::init(const RecorderConfig& config) noexcept {
    if (initialised()) {
        return Status::Duplicate;
    }
    if (config.shards.empty() || config.shards.size() > core::kMaxShards ||
        config.fingerprints.size() != config.shards.size()) {
        return Status::InvalidArgument;
    }
    if (config.pin) {
        sys::HotThreadReport report;
        const sys::HotThreadConfig thread{.cpu = config.cpu, .name = "planck-journal"};
        if (const Status status = sys::configure_hot_thread(thread, report); !ok(status)) {
            return status;
        }
    }

    for (std::size_t i = 0; i < config.shards.size(); ++i) {
        engine::Shard* const shard = config.shards[i];
        if (shard == nullptr || !shard->initialised() || !shard->journaling()) {
            return Status::InvalidArgument;
        }
        Path path;
        if (const Status status = shard_path(config.directory, shard->id(), path); !ok(status)) {
            return status;
        }
        Stream& stream = streams_[i];
        if (const Status status = stream.writer.open(path.c_str(), shard->id(),
                                                     config.fingerprints[i]);
            !ok(status)) {
            return status;
        }
        // The file and the shard must agree on where they are. If they do
        // not, the shard was not rebuilt from this file, and appending to it
        // would produce a journal that replays into something else.
        if (stream.writer.next_sequence() != (shard->sequence() + 1)) {
            return Status::Protocol;
        }
        stream.shard = shard;
        stream.unsynced = false;
        stats_.truncated_bytes += stream.writer.truncated_bytes();
    }

    sync_interval_ns_ = config.sync_interval.raw();
    last_sync_ns_ = sys::monotonic_now().raw();
    idle_ = config.idle;
    stream_count_ = config.shards.size();
    return Status::Ok;
}

std::size_t Recorder::poll() noexcept {
    PLANCK_EXPECT(initialised());

    std::size_t taken = 0;
    for (std::size_t i = 0; i < stream_count_; ++i) {
        Stream& stream = streams_[i];
        const std::size_t drained = stream.shard->journal().drain(
            kMaxRecordsPerPoll, [this, &stream](const engine::JournalEntry& entry) noexcept {
                if (failed_) {
                    ++stats_.discarded;
                    return;
                }
                if (const Status status = stream.writer.append(entry); !ok(status)) {
                    fail(status);
                    ++stats_.discarded;
                    return;
                }
                ++stats_.records;
            });
        taken += drained;
        if (drained == 0 || failed_) {
            continue;
        }
        // One write per shard per pass, however many records arrived. Under
        // load the batches grow on their own.
        const std::size_t pending = stream.writer.buffered_bytes();
        if (const Status status = stream.writer.flush(); !ok(status)) {
            fail(status);
            continue;
        }
        stats_.bytes += pending;
        ++stats_.writes;
        stream.unsynced = true;
    }

    if (!failed_) {
        const std::uint64_t now = sys::monotonic_now().raw();
        if (sync_interval_ns_ == 0 || (now - last_sync_ns_) >= sync_interval_ns_) {
            sync_all();
            last_sync_ns_ = now;
        }
    }
    return taken;
}

void Recorder::run(const std::atomic<bool>& stop) noexcept {
    PLANCK_EXPECT(initialised());
    while (!stop.load(std::memory_order_relaxed)) {
        if (poll() == 0) {
            idle();
        }
    }
    finish();
}

void Recorder::finish() noexcept {
    PLANCK_EXPECT(initialised());
    constexpr int kMaxPasses = 100'000;
    for (int pass = 0; pass < kMaxPasses && poll() != 0; ++pass) {
    }
    if (!failed_) {
        sync_all();
    }
}

void Recorder::sync_all() noexcept {
    for (std::size_t i = 0; i < stream_count_ && !failed_; ++i) {
        Stream& stream = streams_[i];
        if (!stream.unsynced) {
            continue;
        }
        if (const Status status = stream.writer.sync(); !ok(status)) {
            fail(status);
            return;
        }
        stream.unsynced = false;
        ++stats_.syncs;
    }
}

void Recorder::fail(Status status) noexcept {
    if (!failed_) {
        failed_ = true;
        error_ = status;
    }
}

void Recorder::idle() const noexcept {
    if (idle_ == engine::IdlePolicy::Spin) {
        core::cpu_relax();
    } else {
        static_cast<void>(::sched_yield());
    }
}

}  // namespace planck::journal
