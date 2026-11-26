#ifndef PLANCK_JOURNAL_WRITER_HPP
#define PLANCK_JOURNAL_WRITER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/engine/messages.hpp"
#include "planck/journal/format.hpp"

namespace planck::journal {

// Appends records to one shard's journal file.
//
// append() only copies into a buffer. flush() hands the buffer to the
// kernel, after which the records survive this process dying. sync() also
// waits for the disk, after which they survive the machine losing power.
// Nothing here is meant for a matching thread: a recorder thread owns it.
class Writer {
public:
    // 64 KiB, a whole number of records.
    static constexpr std::size_t kBufferRecords = 1365;

    Writer() noexcept = default;
    ~Writer() noexcept { close(); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // Open a shard's journal, creating it if it is not there.
    //
    // An existing file is read from start to end first. It must belong to
    // this shard and carry this fingerprint (InvalidArgument if not). A torn
    // tail left by a crash is cut off, and truncated_bytes() says how much
    // went. Damage with good records after it is refused with Protocol and
    // the file is left exactly as it was.
    [[nodiscard]] core::Status open(const char* path, core::ShardId shard,
                                    std::uint64_t fingerprint) noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    // Buffer one record, writing the buffer out first if it is full. The
    // entry's sequence must be next_sequence(): Protocol if not, and nothing
    // is buffered.
    [[nodiscard]] core::Status append(const engine::JournalEntry& entry) noexcept;

    // Hand everything buffered to the kernel.
    [[nodiscard]] core::Status flush() noexcept;
    // flush(), then wait until it is on the disk.
    [[nodiscard]] core::Status sync() noexcept;

    // The sequence number the next record must carry.
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
    // Size of the file, counting what is still in the buffer.
    [[nodiscard]] std::uint64_t bytes() const noexcept { return file_bytes_ + buffered_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffered_; }
    // Bytes of torn tail removed by open().
    [[nodiscard]] std::uint64_t truncated_bytes() const noexcept { return truncated_; }

private:
    int fd_ = -1;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t file_bytes_ = 0;
    std::uint64_t truncated_ = 0;
    std::size_t buffered_ = 0;
    std::array<std::byte, kBufferRecords * sizeof(Record)> buffer_{};
};

}  // namespace planck::journal

#endif  // PLANCK_JOURNAL_WRITER_HPP
