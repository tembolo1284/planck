#include "planck/journal/writer.hpp"

#include <cerrno>
#include <cstring>
#include <memory>

#include <fcntl.h>
#include <unistd.h>

#include "planck/core/contract.hpp"
#include "planck/journal/reader.hpp"
#include "planck/sys/clock.hpp"
#include "planck/sys/error.hpp"

namespace planck::journal {

namespace {

using core::ok;
using core::Status;

// Write all of it, or fail. A short write to a regular file means the disk
// is full or broken, and either way the caller needs to know.
Status write_all(int fd, const std::byte* from, std::size_t bytes) noexcept {
    std::size_t done = 0;
    // Each pass either makes progress or ends the loop.
    while (done < bytes) {
        const ssize_t put = ::write(fd, from + done, bytes - done);
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put < 0) {
            return sys::status_from_errno(errno);
        }
        if (put == 0) {
            return Status::Io;
        }
        done += static_cast<std::size_t>(put);
    }
    return Status::Ok;
}

struct Existing {
    std::uint64_t last_sequence = 0;
    std::uint64_t valid_bytes = 0;
    std::uint64_t file_bytes = 0;
};

// Check an existing journal end to end. NotFound if there is none.
Status examine(const char* path, core::ShardId shard, std::uint64_t fingerprint,
               Existing& out) noexcept {
    // The reader carries a buffer too big for the stack of a thread that
    // may have been given a small one. This runs once, at startup.
    const std::unique_ptr<Reader> reader{new Reader};
    if (const Status status = reader->open(path); !ok(status)) {
        return status;
    }
    if (reader->header().shard != shard.raw() || reader->header().fingerprint != fingerprint) {
        return Status::InvalidArgument;
    }
    engine::JournalEntry entry;
    Status status = Status::Ok;
    // One record per pass, so the file's length bounds this.
    while (ok(status)) {
        status = reader->next(entry);
    }
    if (status == Status::Protocol && reader->records_after_damage()) {
        return Status::Protocol;
    }
    if (status != Status::Closed && status != Status::Protocol) {
        return status;
    }
    out = Existing{.last_sequence = reader->last_sequence(),
                   .valid_bytes = reader->valid_bytes(),
                   .file_bytes = reader->file_bytes()};
    return Status::Ok;
}

}  // namespace

Status Writer::open(const char* path, core::ShardId shard, std::uint64_t fingerprint) noexcept {
    PLANCK_EXPECT(path != nullptr);
    if (is_open()) {
        return Status::Duplicate;
    }

    Existing existing;
    const Status found = examine(path, shard, fingerprint, existing);
    const bool fresh = (found == Status::NotFound);
    if (!fresh && !ok(found)) {
        return found;
    }

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return sys::status_from_errno(errno);
    }
    Status status = Status::Ok;
    if (fresh) {
        const FileHeader header = make_header(shard, fingerprint, sys::wall_now().raw());
        status = write_all(fd, reinterpret_cast<const std::byte*>(&header), sizeof(header));
        existing = Existing{.last_sequence = 0,
                            .valid_bytes = sizeof(header),
                            .file_bytes = sizeof(header)};
    } else if (existing.file_bytes > existing.valid_bytes) {
        // A torn tail. Cut it off so new records follow the last good one.
        if (::ftruncate(fd, static_cast<off_t>(existing.valid_bytes)) != 0) {
            status = sys::status_from_errno(errno);
        }
    }
    if (ok(status) && ::lseek(fd, static_cast<off_t>(existing.valid_bytes), SEEK_SET) < 0) {
        status = sys::status_from_errno(errno);
    }
    // Make the header, or the cut, durable before anything is built on it.
    if (ok(status) && ::fdatasync(fd) != 0) {
        status = sys::status_from_errno(errno);
    }
    if (!ok(status)) {
        static_cast<void>(::close(fd));
        return status;
    }

    fd_ = fd;
    next_sequence_ = existing.last_sequence + 1;
    file_bytes_ = existing.valid_bytes;
    truncated_ = existing.file_bytes - existing.valid_bytes;
    buffered_ = 0;
    return Status::Ok;
}

void Writer::close() noexcept {
    if (fd_ >= 0) {
        // Last chance to get buffered records out. There is nobody to tell
        // if it fails.
        static_cast<void>(flush());
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
}

Status Writer::append(const engine::JournalEntry& entry) noexcept {
    PLANCK_EXPECT(is_open());
    if (entry.sequence != next_sequence_) {
        return Status::Protocol;
    }
    if (buffered_ == buffer_.size()) {
        if (const Status status = flush(); !ok(status)) {
            return status;
        }
    }
    const Record record = encode(entry);
    std::memcpy(buffer_.data() + buffered_, &record, sizeof(record));
    buffered_ += sizeof(record);
    ++next_sequence_;
    return Status::Ok;
}

Status Writer::flush() noexcept {
    PLANCK_EXPECT(is_open());
    if (buffered_ == 0) {
        return Status::Ok;
    }
    if (const Status status = write_all(fd_, buffer_.data(), buffered_); !ok(status)) {
        return status;
    }
    file_bytes_ += buffered_;
    buffered_ = 0;
    return Status::Ok;
}

Status Writer::sync() noexcept {
    PLANCK_EXPECT(is_open());
    if (const Status status = flush(); !ok(status)) {
        return status;
    }
    if (::fdatasync(fd_) != 0) {
        return sys::status_from_errno(errno);
    }
    return Status::Ok;
}

}  // namespace planck::journal
