#ifndef PLANCK_JOURNAL_READER_HPP
#define PLANCK_JOURNAL_READER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "planck/core/status.hpp"
#include "planck/engine/messages.hpp"
#include "planck/journal/format.hpp"

namespace planck::journal {

// Reads a journal file from the start, one record at a time, checking each.
//
// A journal can end in two ways. Cleanly, at the last byte of a good
// record. Or in damage: a record that fails its checksum, holds nonsense,
// is out of sequence, or is cut short. Damage at the very end is the normal
// mark of a crash, where the last write never fully reached the disk, and
// is safe to cut off. Damage with good records after it is something else,
// and nothing should be cut off without a person looking.
class Reader {
public:
    Reader() noexcept = default;
    ~Reader() noexcept { close(); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // NotFound if there is no such file. Protocol if it is not a planck
    // journal or its header is damaged. Unsupported if it is from another
    // version of the format.
    [[nodiscard]] core::Status open(const char* path) noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    [[nodiscard]] const FileHeader& header() const noexcept { return header_; }

    // The next record.
    //   Ok        out holds it.
    //   Closed    the clean end of the file.
    //   Protocol  damage. Everything up to valid_bytes() is good.
    //   Io        the file could not be read.
    // Once it has returned anything but Ok it keeps returning the same.
    [[nodiscard]] core::Status next(engine::JournalEntry& out) noexcept;

    // After next() has returned Protocol: is there a good record anywhere
    // past the damage? False means the damage is a torn tail.
    [[nodiscard]] bool records_after_damage() noexcept;

    // Sequence number of the last good record read. Zero before the first.
    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
    // Offset just past the last good record read.
    [[nodiscard]] std::uint64_t valid_bytes() const noexcept { return valid_bytes_; }
    [[nodiscard]] std::uint64_t file_bytes() const noexcept { return file_bytes_; }

private:
    static constexpr std::size_t kBufferRecords = 1024;

    // Load the record at offset_ into record. Ok, Closed at end of file,
    // Protocol if fewer bytes than one record remain, or Io.
    [[nodiscard]] core::Status load(Record& record) noexcept;

    int fd_ = -1;
    core::Status state_ = core::Status::Closed;
    FileHeader header_{};
    std::uint64_t last_sequence_ = 0;
    std::uint64_t valid_bytes_ = 0;
    std::uint64_t file_bytes_ = 0;
    // Where the next record to hand out starts.
    std::uint64_t offset_ = 0;
    // File offset of buffer_[0], and how many bytes of it are filled.
    std::uint64_t buffer_offset_ = 0;
    std::size_t buffer_length_ = 0;
    std::array<std::byte, kBufferRecords * sizeof(Record)> buffer_{};
};

}  // namespace planck::journal

#endif  // PLANCK_JOURNAL_READER_HPP
