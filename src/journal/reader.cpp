#include "planck/journal/reader.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "planck/core/contract.hpp"
#include "planck/sys/error.hpp"

namespace planck::journal {

namespace {

using core::ok;
using core::Status;

// Read exactly as much as asked for, or as much as the file has. Returns the
// byte count, or -1 on error.
ssize_t read_at(int fd, std::byte* into, std::size_t bytes, std::uint64_t offset) noexcept {
    std::size_t done = 0;
    // Each pass either makes progress or ends the loop.
    while (done < bytes) {
        const ssize_t got =
            ::pread(fd, into + done, bytes - done, static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    return static_cast<ssize_t>(done);
}

}  // namespace

Status Reader::open(const char* path) noexcept {
    PLANCK_EXPECT(path != nullptr);
    close();
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return sys::status_from_errno(errno);
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        const Status status = sys::status_from_errno(errno);
        static_cast<void>(::close(fd));
        return status;
    }

    FileHeader header;
    const ssize_t got =
        read_at(fd, reinterpret_cast<std::byte*>(&header), sizeof(header), 0);
    Status status = Status::Ok;
    if (got < 0) {
        status = Status::Io;
    } else if (static_cast<std::size_t>(got) != sizeof(header)) {
        status = Status::Protocol;  // too short to be a journal
    } else {
        status = check_header(header);
    }
    if (!ok(status)) {
        static_cast<void>(::close(fd));
        return status;
    }

    fd_ = fd;
    header_ = header;
    state_ = Status::Ok;
    last_sequence_ = 0;
    file_bytes_ = static_cast<std::uint64_t>(info.st_size);
    offset_ = sizeof(FileHeader);
    valid_bytes_ = offset_;
    buffer_offset_ = 0;
    buffer_length_ = 0;
    return Status::Ok;
}

void Reader::close() noexcept {
    if (fd_ >= 0) {
        static_cast<void>(::close(fd_));
        fd_ = -1;
    }
    state_ = Status::Closed;
}

Status Reader::load(Record& record) noexcept {
    const bool buffered = offset_ >= buffer_offset_ &&
                          (offset_ + sizeof(Record)) <= (buffer_offset_ + buffer_length_);
    if (!buffered) {
        const ssize_t got = read_at(fd_, buffer_.data(), buffer_.size(), offset_);
        if (got < 0) {
            return Status::Io;
        }
        buffer_offset_ = offset_;
        buffer_length_ = static_cast<std::size_t>(got);
        if (buffer_length_ == 0) {
            return Status::Closed;
        }
        if (buffer_length_ < sizeof(Record)) {
            return Status::Protocol;  // the file ends part way through a record
        }
    }
    std::memcpy(&record, buffer_.data() + (offset_ - buffer_offset_), sizeof(Record));
    return Status::Ok;
}

Status Reader::next(engine::JournalEntry& out) noexcept {
    PLANCK_EXPECT(is_open());
    if (!ok(state_)) {
        return state_;
    }
    Record record;
    Status status = load(record);
    if (ok(status)) {
        engine::JournalEntry entry;
        // Records are numbered from 1 with no gaps. Anything else means a
        // piece of the file is missing or has been overwritten.
        if (decode(record, entry) && entry.sequence == (last_sequence_ + 1)) {
            out = entry;
            last_sequence_ = entry.sequence;
            offset_ += sizeof(Record);
            valid_bytes_ = offset_;
            return Status::Ok;
        }
        status = Status::Protocol;
    }
    state_ = status;
    return status;
}

bool Reader::records_after_damage() noexcept {
    PLANCK_EXPECT(is_open());
    PLANCK_EXPECT(state_ == Status::Protocol);

    // Records sit at fixed offsets, so walk on from the damaged one and see
    // whether anything later still checks out. The file's length bounds the
    // walk.
    const std::uint64_t resume = offset_;
    bool found = false;
    offset_ += sizeof(Record);
    while (!found && (offset_ + sizeof(Record)) <= file_bytes_) {
        Record record;
        if (!ok(load(record))) {
            break;
        }
        engine::JournalEntry entry;
        found = decode(record, entry) && entry.sequence > last_sequence_;
        offset_ += sizeof(Record);
    }
    offset_ = resume;
    return found;
}

}  // namespace planck::journal
