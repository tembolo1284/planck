#include "planck/journal/format.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#include "planck/core/config.hpp"

namespace planck::journal {

namespace {

using core::Status;

// One table entry per byte value, built at compile time.
constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
    constexpr std::uint32_t kPolynomial = 0x82F63B78U;  // Castagnoli, bit-reversed
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t value = 0; value < 256; ++value) {
        std::uint32_t crc = value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = ((crc & 1U) != 0) ? ((crc >> 1) ^ kPolynomial) : (crc >> 1);
        }
        table[value] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

template <typename T>
std::uint32_t checksum_of(const T& value) noexcept {
    // Everything except the trailing checksum field itself.
    const auto* const bytes = reinterpret_cast<const std::byte*>(&value);
    return crc32c({bytes, sizeof(T) - sizeof(std::uint32_t)});
}

// FNV-1a, fed a few integers at a time.
struct Hasher {
    std::uint64_t state = 0xCBF29CE484222325ULL;
    void add(std::uint64_t value) noexcept {
        for (int byte = 0; byte < 8; ++byte) {
            state ^= (value >> (8 * byte)) & 0xFFU;
            state *= 0x100000001B3ULL;
        }
    }
};

}  // namespace

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = ~std::uint32_t{0};
    for (const std::byte b : bytes) {
        crc = kCrcTable[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

Record encode(const engine::JournalEntry& entry) noexcept {
    const engine::Command& command = entry.command;
    Record record{.sequence = entry.sequence,
                  .timestamp_ns = entry.timestamp_ns,
                  .id = command.id,
                  .price = command.price.raw(),
                  .qty = command.qty.raw(),
                  .symbol = command.symbol.raw(),
                  .session = command.session.raw(),
                  .kind = std::to_underlying(command.kind),
                  .side = std::to_underlying(command.side),
                  .type = std::to_underlying(command.type),
                  .tif = std::to_underlying(command.tif),
                  .reserved = 0,
                  .checksum = 0};
    record.checksum = checksum_of(record);
    return record;
}

bool decode(const Record& record, engine::JournalEntry& out) noexcept {
    if (record.checksum != checksum_of(record)) {
        return false;
    }
    // The checksum says the bytes are the ones that were written. These say
    // that what was written makes sense. A shard trusts its commands, so
    // nothing may reach one that a gateway could not have produced.
    const auto side = static_cast<core::Side>(record.side);
    const auto type = static_cast<core::OrderType>(record.type);
    const auto tif = static_cast<core::TimeInForce>(record.tif);
    const bool sane = record.sequence != 0 && record.timestamp_ns != 0 && record.reserved == 0 &&
                      record.kind <= std::to_underlying(engine::CommandKind::CancelOrder) &&
                      core::is_valid(side) && core::is_valid(type) && core::is_valid(tif);
    if (!sane) {
        return false;
    }
    out = engine::JournalEntry{
        .sequence = record.sequence,
        .timestamp_ns = record.timestamp_ns,
        .command = engine::Command{.ingress_tsc = 0,
                                   .id = record.id,
                                   .price = core::Price{record.price},
                                   .qty = core::Qty{record.qty},
                                   .symbol = core::SymbolId{record.symbol},
                                   .session = core::SessionId{record.session},
                                   .kind = static_cast<engine::CommandKind>(record.kind),
                                   .side = side,
                                   .type = type,
                                   .tif = tif}};
    return true;
}

FileHeader make_header(core::ShardId shard, std::uint64_t fingerprint,
                       std::uint64_t created_ns) noexcept {
    FileHeader header;
    header.shard = shard.raw();
    header.fingerprint = fingerprint;
    header.created_ns = created_ns;
    header.checksum = checksum_of(header);
    return header;
}

Status check_header(const FileHeader& header) noexcept {
    if (header.magic != kMagic || header.checksum != checksum_of(header)) {
        return Status::Protocol;
    }
    if (header.version != kVersion || header.header_bytes != sizeof(FileHeader)) {
        return Status::Unsupported;
    }
    return Status::Ok;
}

std::uint64_t fingerprint(const engine::ShardConfig& config) noexcept {
    Hasher hasher;
    hasher.add(kVersion);
    hasher.add(config.shard.raw());
    hasher.add(config.order_capacity);
    hasher.add(config.symbols.size());
    for (const engine::SymbolConfig& symbol : config.symbols) {
        hasher.add(symbol.symbol.raw());
        hasher.add(static_cast<std::uint64_t>(static_cast<std::int64_t>(symbol.band_low.raw())));
        hasher.add(symbol.band_ticks);
    }
    return hasher.state;
}

Status shard_path(std::string_view directory, core::ShardId shard, Path& out) noexcept {
    constexpr std::size_t kNameBytes = 32;  // "/shard-NN.journal" and the terminator, with room
    if (directory.empty() || directory.size() > (out.chars.size() - kNameBytes)) {
        return Status::InvalidArgument;
    }
    const int written = std::snprintf(out.chars.data(), out.chars.size(), "%.*s/shard-%02u.journal",
                                      static_cast<int>(directory.size()), directory.data(),
                                      static_cast<unsigned>(shard.raw()));
    if (written <= 0 || static_cast<std::size_t>(written) >= out.chars.size()) {
        return Status::InvalidArgument;
    }
    return Status::Ok;
}

}  // namespace planck::journal
