#ifndef PLANCK_JOURNAL_FORMAT_HPP
#define PLANCK_JOURNAL_FORMAT_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/engine/messages.hpp"
#include "planck/engine/shard.hpp"

// The journal file: one per shard, a 64-byte header followed by 48-byte
// records, one for every command the shard handled, in order. Integers are
// little-endian. Every record carries a checksum, so a file cut short or
// damaged by a crash is detected where it happens and not three orders
// later.
namespace planck::journal {

static_assert(std::endian::native == std::endian::little,
              "the journal format is little-endian and does no byte swapping");

inline constexpr std::array<char, 8> kMagic = {'P', 'L', 'N', 'K', 'J', 'R', 'N', 'L'};
// Version 2: a record's session is the client id its owner logs in with, the
// same on every connection. In version 1 it was a number given to one
// connection, so a version 1 journal cannot say who owns what and is refused.
// Version 3: two more kinds of record, for a session that lost its
// connection and for all of them at once.
inline constexpr std::uint32_t kVersion = 3;

struct FileHeader {
    std::array<char, 8> magic = kMagic;
    std::uint32_t version = kVersion;
    std::uint16_t shard = 0;
    std::uint16_t header_bytes = 64;
    // Identifies the shard configuration the commands were run against.
    // Replaying into a different configuration would rebuild different books.
    std::uint64_t fingerprint = 0;
    // Wall-clock time the file was created, nanoseconds since the Unix epoch.
    std::uint64_t created_ns = 0;
    std::array<std::uint8_t, 28> reserved{};
    // CRC-32C of the 60 bytes before this field.
    std::uint32_t checksum = 0;
};

struct Record {
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
    // NewOrder: the client's order id. CancelOrder: the venue order id.
    std::uint64_t id = 0;
    std::int32_t price = 0;
    std::uint32_t qty = 0;
    std::uint16_t symbol = 0;
    std::uint16_t session = 0;
    std::uint8_t kind = 0;
    std::uint8_t side = 0;
    std::uint8_t type = 0;
    std::uint8_t tif = 0;
    std::uint32_t reserved = 0;
    // CRC-32C of the 44 bytes before this field.
    std::uint32_t checksum = 0;
};

static_assert(sizeof(FileHeader) == 64 && std::is_trivially_copyable_v<FileHeader>);
static_assert(sizeof(Record) == 48 && std::is_trivially_copyable_v<Record>);
static_assert(std::has_unique_object_representations_v<FileHeader>, "no padding in the header");
static_assert(std::has_unique_object_representations_v<Record>, "no padding in a record");

// CRC-32C (Castagnoli), the checksum used by iSCSI and ext4. Catches every
// burst error up to 32 bits and all the common ways a write goes wrong.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

// A record ready to write, checksum included.
[[nodiscard]] Record encode(const engine::JournalEntry& entry) noexcept;

// Turn a record read from disk back into an entry. False, leaving out
// untouched, if the checksum is wrong or any field holds a value no shard
// could have written.
[[nodiscard]] bool decode(const Record& record, engine::JournalEntry& out) noexcept;

// A header ready to write, checksum included.
[[nodiscard]] FileHeader make_header(core::ShardId shard, std::uint64_t fingerprint,
                                     std::uint64_t created_ns) noexcept;

// Ok if the header is intact and from this version. Protocol if it is not a
// planck journal or is damaged. Unsupported if it is from another version.
[[nodiscard]] core::Status check_header(const FileHeader& header) noexcept;

// A number that changes whenever anything that affects matching changes:
// the shard's id, its order capacity, and its symbols with their price
// bands, in order.
[[nodiscard]] std::uint64_t fingerprint(const engine::ShardConfig& config) noexcept;

// A file path as a C string, without the heap.
struct Path {
    std::array<char, 512> chars{};
    [[nodiscard]] const char* c_str() const noexcept { return chars.data(); }
};

// "<directory>/shard-NN.journal". InvalidArgument if directory is empty or
// too long.
[[nodiscard]] core::Status shard_path(std::string_view directory, core::ShardId shard,
                                      Path& out) noexcept;

}  // namespace planck::journal

#endif  // PLANCK_JOURNAL_FORMAT_HPP
