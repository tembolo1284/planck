// planck-replay: read a journal file, check it, and say what is in it.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>

#include "planck/core/types.hpp"
#include "planck/journal/format.hpp"
#include "planck/journal/reader.hpp"
#include "support.hpp"

namespace {

using planck::core::ok;
using planck::core::Status;
namespace apps = planck::apps;
namespace core = planck::core;
namespace engine = planck::engine;
namespace journal = planck::journal;

constexpr char kUsage[] =
    "planck-replay: check a planck journal and show what is in it\n"
    "\n"
    "  --file PATH   the journal, e.g. journal/shard-00.journal\n"
    "  --dump        print every command, one per line\n"
    "\n"
    "Exit status: 0 if the journal is intact, 3 if it ends in a torn record (which a\n"
    "restart will drop), 4 if it is damaged before its end, 1 if it cannot be read.\n";

void print(const engine::JournalEntry& entry) noexcept {
    const engine::Command& c = entry.command;
    const auto sequence = static_cast<unsigned long long>(entry.sequence);
    const apps::ShortText time = apps::format_time(entry.timestamp_ns);
    if (c.kind == engine::CommandKind::CancelOrder) {
        std::printf("%llu %s session=%u cancel order_id=%llu\n", sequence, time.c_str(),
                    c.session.raw(), static_cast<unsigned long long>(c.id));
        return;
    }
    const char* const side = (c.side == core::Side::Buy) ? "buy" : "sell";
    const char* const tif = (c.tif == core::TimeInForce::Day)
                                ? "day"
                                : (c.tif == core::TimeInForce::ImmediateOrCancel) ? "ioc" : "fok";
    if (c.type == core::OrderType::Market) {
        std::printf("%llu %s session=%u %s symbol=%u qty=%u market client_order_id=%llu\n", sequence,
                    time.c_str(), c.session.raw(), side, c.symbol.raw(), c.qty.raw(),
                    static_cast<unsigned long long>(c.id));
    } else {
        std::printf("%llu %s session=%u %s symbol=%u qty=%u price=%d %s client_order_id=%llu\n",
                    sequence, time.c_str(), c.session.raw(), side, c.symbol.raw(), c.qty.raw(),
                    c.price.raw(), tif, static_cast<unsigned long long>(c.id));
    }
}

}  // namespace

int main(int argc, char** argv) {
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    apps::Args args(argc, argv);
    if (args.flag("--help")) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    std::string_view file;
    const bool dump = args.flag("--dump");
    if (!args.text("--file", file) || !args.finish()) {
        return 2;
    }
    if (file.empty()) {
        std::fprintf(stderr, "planck-replay: --file is required (try --help)\n");
        return 2;
    }

    // The reader's buffer is large, so keep it off the stack. The path came
    // from argv, so it ends in a terminator.
    const std::unique_ptr<journal::Reader> reader{new journal::Reader};
    if (const Status status = reader->open(file.data()); !ok(status)) {
        const std::string_view reason = core::to_string(status);
        std::fprintf(stderr, "planck-replay: cannot read %s: %.*s\n", file.data(),
                     static_cast<int>(reason.size()), reason.data());
        return 1;
    }
    const journal::FileHeader& header = reader->header();
    std::printf("journal shard=%u version=%u fingerprint=%016llx created=%s bytes=%llu\n",
                header.shard, header.version, static_cast<unsigned long long>(header.fingerprint),
                apps::format_time(header.created_ns).c_str(),
                static_cast<unsigned long long>(reader->file_bytes()));

    std::uint64_t new_orders = 0;
    std::uint64_t cancels = 0;
    std::uint64_t first_time = 0;
    std::uint64_t last_time = 0;
    engine::JournalEntry entry;
    Status status = reader->next(entry);
    // One record per pass, so the file's length bounds this.
    while (ok(status)) {
        if (dump) {
            print(entry);
        }
        ((entry.command.kind == engine::CommandKind::NewOrder) ? new_orders : cancels) += 1;
        first_time = (first_time == 0) ? entry.timestamp_ns : first_time;
        last_time = entry.timestamp_ns;
        status = reader->next(entry);
    }

    std::printf("commands=%llu new_orders=%llu cancels=%llu first=%s last=%s\n",
                static_cast<unsigned long long>(reader->last_sequence()),
                static_cast<unsigned long long>(new_orders), static_cast<unsigned long long>(cancels),
                apps::format_time(first_time).c_str(), apps::format_time(last_time).c_str());

    if (status == Status::Closed) {
        std::printf("intact\n");
        return 0;
    }
    if (status == Status::Protocol) {
        const auto unread =
            static_cast<unsigned long long>(reader->file_bytes() - reader->valid_bytes());
        if (reader->records_after_damage()) {
            std::printf("DAMAGED after command %llu, at byte %llu, with good records after it. "
                        "%llu bytes unread. A venue will refuse to start from this file.\n",
                        static_cast<unsigned long long>(reader->last_sequence()),
                        static_cast<unsigned long long>(reader->valid_bytes()), unread);
            return 4;
        }
        std::printf("torn tail: the last %llu bytes are not a whole record. A restart drops "
                    "them and carries on from command %llu.\n",
                    unread, static_cast<unsigned long long>(reader->last_sequence()));
        return 3;
    }
    std::fprintf(stderr, "planck-replay: read error\n");
    return 1;
}
