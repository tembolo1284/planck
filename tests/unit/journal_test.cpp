#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "planck/journal/format.hpp"
#include "planck/journal/reader.hpp"
#include "planck/journal/writer.hpp"
#include "support/shard_harness.hpp"
#include "support/temp_dir.hpp"

namespace planck::journal {
namespace {

using core::Status;

constexpr core::ShardId kShard{2};
constexpr std::uint64_t kFingerprint = 0xABCDEF0123456789ULL;

engine::JournalEntry entry_of(std::uint64_t sequence) {
    engine::JournalEntry entry;
    entry.sequence = sequence;
    entry.timestamp_ns = 1'700'000'000'000'000'000ULL + sequence;
    entry.command = test::new_order(static_cast<std::uint16_t>(1 + (sequence % 5)), 3,
                                    (sequence % 2 == 0) ? core::Side::Buy : core::Side::Sell,
                                    950 + static_cast<std::int32_t>(sequence % 40),
                                    1 + static_cast<std::uint32_t>(sequence % 9), 5000 + sequence);
    entry.command.ingress_tsc = 0;  // not stored, so not expected back
    return entry;
}

bool same(const engine::JournalEntry& a, const engine::JournalEntry& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

class JournalFile : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.ok());
        path_ = dir_.file("shard-02.journal");
    }

    // Write records first..last to the file and close it.
    void write(std::uint64_t first, std::uint64_t last) {
        const auto writer = std::make_unique<Writer>();
        ASSERT_EQ(writer->open(path_.c_str(), kShard, kFingerprint), Status::Ok);
        ASSERT_EQ(writer->next_sequence(), first);
        for (std::uint64_t sequence = first; sequence <= last; ++sequence) {
            ASSERT_EQ(writer->append(entry_of(sequence)), Status::Ok);
        }
        ASSERT_EQ(writer->sync(), Status::Ok);
    }

    // Read the whole file. Returns the status that ended the read.
    Status read_all(std::vector<engine::JournalEntry>& out, Reader& reader) {
        const Status opened = reader.open(path_.c_str());
        if (opened != Status::Ok) {
            return opened;
        }
        engine::JournalEntry entry;
        Status status = reader.next(entry);
        while (status == Status::Ok) {
            out.push_back(entry);
            status = reader.next(entry);
        }
        return status;
    }

    std::uintmax_t size() const { return std::filesystem::file_size(path_); }

    // Overwrite bytes of the file in place.
    void poke(std::uintmax_t offset, std::string_view bytes) {
        std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file.good());
        file.seekp(static_cast<std::streamoff>(offset));
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(file.good());
    }

    static constexpr std::uintmax_t offset_of(std::uint64_t sequence) {
        return sizeof(FileHeader) + ((sequence - 1) * sizeof(Record));
    }

    test::TempDir dir_;
    std::string path_;
};

TEST(JournalFormat, ChecksumMatchesThePublishedTestVector) {
    const std::string_view text = "123456789";
    EXPECT_EQ(crc32c({reinterpret_cast<const std::byte*>(text.data()), text.size()}), 0xE3069283U);
    EXPECT_EQ(crc32c({}), 0U);
}

TEST(JournalFormat, RecordsSurviveTheRoundTrip) {
    for (std::uint64_t sequence = 1; sequence < 50; ++sequence) {
        const engine::JournalEntry original = entry_of(sequence);
        engine::JournalEntry back;
        ASSERT_TRUE(decode(encode(original), back));
        EXPECT_TRUE(same(original, back)) << "record " << sequence;
    }
    engine::JournalEntry cancel = entry_of(9);
    cancel.command = test::cancel_order(4, 0x2000000001000007ULL);
    cancel.command.ingress_tsc = 0;
    engine::JournalEntry back;
    ASSERT_TRUE(decode(encode(cancel), back));
    EXPECT_TRUE(same(cancel, back));
}

TEST(JournalFormat, DecodeRefusesAnythingAShardCouldNotHaveWritten) {
    const Record good = encode(entry_of(5));
    engine::JournalEntry out = entry_of(77);

    // Any single flipped bit is caught by the checksum.
    for (std::size_t bit = 0; bit < sizeof(Record) * 8; ++bit) {
        Record damaged = good;
        auto* const bytes = reinterpret_cast<unsigned char*>(&damaged);
        bytes[bit / 8] = static_cast<unsigned char>(bytes[bit / 8] ^ (1U << (bit % 8)));
        ASSERT_FALSE(decode(damaged, out)) << "accepted a record with bit " << bit << " flipped";
    }
    EXPECT_EQ(out.sequence, 77U) << "output touched on failure";

    // Nonsense with a correct checksum is still nonsense.
    const auto resealed = [](Record record) {
        record.checksum = crc32c({reinterpret_cast<const std::byte*>(&record), sizeof(Record) - 4});
        return record;
    };
    Record bad = good;
    bad.side = 2;
    EXPECT_FALSE(decode(resealed(bad), out));
    bad = good;
    bad.kind = 9;
    EXPECT_FALSE(decode(resealed(bad), out));
    bad = good;
    bad.tif = 3;
    EXPECT_FALSE(decode(resealed(bad), out));
    bad = good;
    bad.sequence = 0;
    EXPECT_FALSE(decode(resealed(bad), out));
    bad = good;
    bad.reserved = 1;
    EXPECT_FALSE(decode(resealed(bad), out));
    EXPECT_TRUE(decode(resealed(good), out)) << "the helper itself must produce valid records";

    const Record zeros{};
    EXPECT_FALSE(decode(zeros, out)) << "a block of zeros is what a torn write often leaves";
}

TEST(JournalFormat, HeaderIsChecked) {
    const FileHeader good = make_header(kShard, kFingerprint, 123);
    EXPECT_EQ(check_header(good), Status::Ok);
    EXPECT_EQ(good.shard, 2);
    EXPECT_EQ(good.fingerprint, kFingerprint);

    FileHeader bad = good;
    bad.magic[0] = 'X';
    EXPECT_EQ(check_header(bad), Status::Protocol);
    bad = good;
    bad.fingerprint ^= 1;
    EXPECT_EQ(check_header(bad), Status::Protocol) << "checksum must cover the fingerprint";
    // Older and newer formats alike. Version 1 gave sessions another meaning.
    for (const std::uint32_t other : {std::uint32_t{1}, kVersion + 1}) {
        bad = make_header(kShard, kFingerprint, 123);
        bad.version = other;
        bad.checksum = crc32c({reinterpret_cast<const std::byte*>(&bad), sizeof(bad) - 4});
        EXPECT_EQ(check_header(bad), Status::Unsupported) << "version " << other;
    }
}

TEST(JournalFormat, FingerprintFollowsEverythingThatAffectsMatching) {
    const engine::ShardConfig base = test::small_shard();
    const std::uint64_t reference = fingerprint(base);
    EXPECT_EQ(fingerprint(test::small_shard()), reference) << "must be stable";

    engine::ShardConfig changed = base;
    changed.shard = core::ShardId{3};
    EXPECT_NE(fingerprint(changed), reference);
    changed = base;
    changed.order_capacity = 8192;
    EXPECT_NE(fingerprint(changed), reference);

    static constexpr engine::SymbolConfig kWiderBand[] = {
        {core::SymbolId{3}, core::Price{900}, 512}, {core::SymbolId{700}, core::Price{-128}, 256}};
    changed = base;
    changed.symbols = kWiderBand;
    EXPECT_NE(fingerprint(changed), reference);
    static constexpr engine::SymbolConfig kShiftedBand[] = {
        {core::SymbolId{3}, core::Price{901}, 256}, {core::SymbolId{700}, core::Price{-128}, 256}};
    changed.symbols = kShiftedBand;
    EXPECT_NE(fingerprint(changed), reference);
    static constexpr engine::SymbolConfig kOneSymbol[] = {{core::SymbolId{3}, core::Price{900}, 256}};
    changed.symbols = kOneSymbol;
    EXPECT_NE(fingerprint(changed), reference);

    // Things that do not affect matching do not change it.
    changed = base;
    changed.command_ring_capacity = 1U << 10;
    changed.journal_ring_capacity = 1U << 10;
    changed.idle = engine::IdlePolicy::Spin;
    EXPECT_EQ(fingerprint(changed), reference);
}

TEST(JournalFormat, BuildsShardPaths) {
    Path path;
    ASSERT_EQ(shard_path("/var/lib/planck", core::ShardId{7}, path), Status::Ok);
    EXPECT_STREQ(path.c_str(), "/var/lib/planck/shard-07.journal");
    ASSERT_EQ(shard_path("data", core::ShardId{15}, path), Status::Ok);
    EXPECT_STREQ(path.c_str(), "data/shard-15.journal");
    EXPECT_EQ(shard_path("", core::ShardId{1}, path), Status::InvalidArgument);
    EXPECT_EQ(shard_path(std::string(600, 'x'), core::ShardId{1}, path), Status::InvalidArgument);
}

TEST_F(JournalFile, WhatIsWrittenIsReadBack) {
    write(1, 3000);  // more than one buffer's worth
    EXPECT_EQ(size(), offset_of(3001));

    const auto reader = std::make_unique<Reader>();
    std::vector<engine::JournalEntry> entries;
    EXPECT_EQ(read_all(entries, *reader), Status::Closed);
    ASSERT_EQ(entries.size(), 3000U);
    for (std::uint64_t sequence = 1; sequence <= 3000; ++sequence) {
        ASSERT_TRUE(same(entries[sequence - 1], entry_of(sequence))) << "record " << sequence;
    }
    EXPECT_EQ(reader->header().shard, 2);
    EXPECT_EQ(reader->header().fingerprint, kFingerprint);
    EXPECT_NE(reader->header().created_ns, 0U);
    EXPECT_EQ(reader->last_sequence(), 3000U);
    EXPECT_EQ(reader->valid_bytes(), size());

    engine::JournalEntry again;
    EXPECT_EQ(reader->next(again), Status::Closed) << "the end stays the end";
}

TEST_F(JournalFile, MissingAndForeignFilesAreToldApart) {
    const auto reader = std::make_unique<Reader>();
    EXPECT_EQ(reader->open(path_.c_str()), Status::NotFound);
    EXPECT_FALSE(reader->is_open());

    { std::ofstream(path_) << "this is not a journal, it is a text file that happens to be long enough"; }
    EXPECT_EQ(reader->open(path_.c_str()), Status::Protocol);
    { std::ofstream(path_, std::ios::trunc) << "short"; }
    EXPECT_EQ(reader->open(path_.c_str()), Status::Protocol);

    const auto writer = std::make_unique<Writer>();
    EXPECT_EQ(writer->open(path_.c_str(), kShard, kFingerprint), Status::Protocol)
        << "a writer must not adopt a file that is not a journal";
    EXPECT_EQ(std::filesystem::file_size(path_), 5U) << "and must leave it alone";
}

TEST_F(JournalFile, BufferedRecordsAreNotInTheFileUntilFlushed) {
    const auto writer = std::make_unique<Writer>();
    ASSERT_EQ(writer->open(path_.c_str(), kShard, kFingerprint), Status::Ok);
    ASSERT_EQ(writer->append(entry_of(1)), Status::Ok);
    ASSERT_EQ(writer->append(entry_of(2)), Status::Ok);
    EXPECT_EQ(size(), sizeof(FileHeader));
    EXPECT_EQ(writer->buffered_bytes(), 2 * sizeof(Record));
    EXPECT_EQ(writer->bytes(), offset_of(3));

    ASSERT_EQ(writer->flush(), Status::Ok);
    EXPECT_EQ(size(), offset_of(3));
    EXPECT_EQ(writer->buffered_bytes(), 0U);
    EXPECT_EQ(writer->flush(), Status::Ok) << "flushing nothing is fine";
}

TEST_F(JournalFile, RecordsMustArriveInSequence) {
    const auto writer = std::make_unique<Writer>();
    ASSERT_EQ(writer->open(path_.c_str(), kShard, kFingerprint), Status::Ok);
    EXPECT_EQ(writer->append(entry_of(2)), Status::Protocol) << "skipped 1";
    ASSERT_EQ(writer->append(entry_of(1)), Status::Ok);
    EXPECT_EQ(writer->append(entry_of(1)), Status::Protocol) << "repeated 1";
    EXPECT_EQ(writer->append(entry_of(3)), Status::Protocol) << "skipped 2";
    EXPECT_EQ(writer->next_sequence(), 2U);
    EXPECT_EQ(writer->buffered_bytes(), sizeof(Record)) << "a refused record must not be buffered";
}

TEST_F(JournalFile, ReopeningContinuesWhereItLeftOff) {
    write(1, 10);
    write(11, 25);
    write(26, 26);
    const auto reader = std::make_unique<Reader>();
    std::vector<engine::JournalEntry> entries;
    EXPECT_EQ(read_all(entries, *reader), Status::Closed);
    ASSERT_EQ(entries.size(), 26U);
    EXPECT_TRUE(same(entries[25], entry_of(26)));
}

TEST_F(JournalFile, WriterRefusesAnotherShardsOrAnotherConfigurationsFile) {
    write(1, 5);
    const auto writer = std::make_unique<Writer>();
    EXPECT_EQ(writer->open(path_.c_str(), core::ShardId{3}, kFingerprint), Status::InvalidArgument);
    EXPECT_EQ(writer->open(path_.c_str(), kShard, kFingerprint + 1), Status::InvalidArgument);
    EXPECT_FALSE(writer->is_open());
    EXPECT_EQ(size(), offset_of(6)) << "a refused file must be left alone";
}

TEST_F(JournalFile, FileCutMidRecordIsATornTail) {
    write(1, 20);
    std::filesystem::resize_file(path_, offset_of(20) + 17);  // record 20 is only part there

    const auto reader = std::make_unique<Reader>();
    std::vector<engine::JournalEntry> entries;
    EXPECT_EQ(read_all(entries, *reader), Status::Protocol);
    EXPECT_EQ(entries.size(), 19U);
    EXPECT_EQ(reader->valid_bytes(), offset_of(20));
    EXPECT_FALSE(reader->records_after_damage());

    // A writer cuts the tail off and carries on from record 20.
    const auto writer = std::make_unique<Writer>();
    ASSERT_EQ(writer->open(path_.c_str(), kShard, kFingerprint), Status::Ok);
    EXPECT_EQ(writer->truncated_bytes(), 17U);
    EXPECT_EQ(writer->next_sequence(), 20U);
    EXPECT_EQ(size(), offset_of(20));
    ASSERT_EQ(writer->append(entry_of(20)), Status::Ok);
    ASSERT_EQ(writer->sync(), Status::Ok);

    entries.clear();
    EXPECT_EQ(read_all(entries, *reader), Status::Closed);
    EXPECT_EQ(entries.size(), 20U);
}

TEST_F(JournalFile, GarbageAtTheEndIsATornTail) {
    write(1, 20);
    // The last three records' worth of file exists but holds zeros, which is
    // what a crash can leave when the size reached the disk and the data did not.
    poke(offset_of(18), std::string(3 * sizeof(Record), '\0'));

    const auto reader = std::make_unique<Reader>();
    std::vector<engine::JournalEntry> entries;
    EXPECT_EQ(read_all(entries, *reader), Status::Protocol);
    EXPECT_EQ(entries.size(), 17U);
    EXPECT_FALSE(reader->records_after_damage());

    const auto writer = std::make_unique<Writer>();
    ASSERT_EQ(writer->open(path_.c_str(), kShard, kFingerprint), Status::Ok);
    EXPECT_EQ(writer->truncated_bytes(), 3 * sizeof(Record));
    EXPECT_EQ(writer->next_sequence(), 18U);
}

TEST_F(JournalFile, DamageInTheMiddleIsNotCutOff) {
    write(1, 20);
    poke(offset_of(8) + 20, "X");  // one byte, twelve records from the end

    const auto reader = std::make_unique<Reader>();
    std::vector<engine::JournalEntry> entries;
    EXPECT_EQ(read_all(entries, *reader), Status::Protocol);
    EXPECT_EQ(entries.size(), 7U);
    EXPECT_TRUE(reader->records_after_damage()) << "records 9 to 20 are still good";
    engine::JournalEntry entry;
    EXPECT_EQ(reader->next(entry), Status::Protocol) << "looking ahead must not move the reader";
    EXPECT_EQ(reader->valid_bytes(), offset_of(8));

    // Cutting here would silently throw away twelve good records.
    const auto writer = std::make_unique<Writer>();
    EXPECT_EQ(writer->open(path_.c_str(), kShard, kFingerprint), Status::Protocol);
    EXPECT_EQ(size(), offset_of(21)) << "the file must be left exactly as it was";
}

TEST_F(JournalFile, AMissingRecordIsDamageEvenIfEveryChecksumIsGood) {
    // Records 1..5, then 7: each is valid on its own, but 6 is gone.
    write(1, 5);
    const Record seven = encode(entry_of(7));
    {
        std::ofstream file(path_, std::ios::app | std::ios::binary);
        file.write(reinterpret_cast<const char*>(&seven), sizeof(seven));
    }
    const auto reader = std::make_unique<Reader>();
    std::vector<engine::JournalEntry> entries;
    EXPECT_EQ(read_all(entries, *reader), Status::Protocol);
    EXPECT_EQ(entries.size(), 5U);
}

TEST_F(JournalFile, EmptyJournalIsValid) {
    write(1, 0);  // header only
    EXPECT_EQ(size(), sizeof(FileHeader));
    const auto reader = std::make_unique<Reader>();
    std::vector<engine::JournalEntry> entries;
    EXPECT_EQ(read_all(entries, *reader), Status::Closed);
    EXPECT_TRUE(entries.empty());
    EXPECT_EQ(reader->last_sequence(), 0U);
}

}  // namespace
}  // namespace planck::journal
