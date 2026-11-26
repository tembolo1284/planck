// planck-mdtap: listen to one market data stream and print what is on it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include <time.h>

#include "planck/core/config.hpp"
#include "planck/core/types.hpp"
#include "planck/feed/receiver.hpp"
#include "planck/sys/clock.hpp"
#include "planck/wire/codec.hpp"
#include "support.hpp"

namespace {

using planck::core::ok;
using planck::core::Status;
namespace apps = planck::apps;
namespace core = planck::core;
namespace feed = planck::feed;
namespace net = planck::net;
namespace sys = planck::sys;
namespace wire = planck::wire;

constexpr char kUsage[] =
    "planck-mdtap: print a planck market data stream\n"
    "\n"
    "  --stream N                 which shard's stream             (0)\n"
    "  --listen ADDRESS:PORT      where the stream arrives. A multicast group to join,\n"
    "                             or a local address               (239.255.42.1:30001 + N)\n"
    "  --retransmit ADDRESS:PORT  the venue's retransmit port, used to recover lost\n"
    "                             messages. Without it a gap is simply reported.\n"
    "  --from-start               replay the stream from its first message.\n"
    "                             Needs --retransmit.\n"
    "  --count N                  exit after N messages            (run until Ctrl-C)\n"
    "  --quiet                    print one summary line a second, not every message\n";

struct Tap {
    std::uint64_t messages = 0;
    std::uint64_t adds = 0;
    std::uint64_t trades = 0;
    std::uint64_t deletes = 0;
    std::uint64_t traded_qty = 0;
    bool quiet = false;

    void operator()(std::uint64_t sequence, std::span<const std::byte> bytes) noexcept {
        const auto u64 = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
        ++messages;
        const wire::Frame frame = wire::next_frame(bytes);
        switch (frame.type) {
            case wire::MsgType::MdAddOrder: {
                wire::MdAddOrder m;
                if (ok(wire::decode(frame.bytes, m))) {
                    ++adds;
                    if (!quiet) {
                        std::printf("%llu add symbol=%u %s qty=%u price=%d order_id=%llu time=%s\n",
                                    u64(sequence), m.symbol_id,
                                    m.side == std::to_underlying(core::Side::Buy) ? "buy" : "sell",
                                    m.qty, m.price, u64(m.order_id),
                                    apps::format_time(m.timestamp).c_str());
                    }
                }
                break;
            }
            case wire::MsgType::MdExecuted: {
                wire::MdExecuted m;
                if (ok(wire::decode(frame.bytes, m))) {
                    ++trades;
                    traded_qty += m.qty;
                    if (!quiet) {
                        std::printf("%llu trade symbol=%u qty=%u price=%d order_id=%llu "
                                    "match_id=%llu time=%s\n",
                                    u64(sequence), m.symbol_id, m.qty, m.price, u64(m.order_id),
                                    u64(m.match_id), apps::format_time(m.timestamp).c_str());
                    }
                }
                break;
            }
            case wire::MsgType::MdDeleted: {
                wire::MdDeleted m;
                if (ok(wire::decode(frame.bytes, m))) {
                    ++deletes;
                    if (!quiet) {
                        std::printf("%llu delete symbol=%u order_id=%llu time=%s\n", u64(sequence),
                                    m.symbol_id, u64(m.order_id),
                                    apps::format_time(m.timestamp).c_str());
                    }
                }
                break;
            }
            default: {
                const std::string_view name = wire::to_string(frame.type);
                std::printf("%llu unexpected message %.*s\n", u64(sequence),
                            static_cast<int>(name.size()), name.data());
                break;
            }
        }
    }
};

void print_summary(const Tap& tap, const feed::ReceiverStats& stats) noexcept {
    const auto u64 = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("summary messages=%llu adds=%llu trades=%llu deletes=%llu traded_qty=%llu "
                "datagrams=%llu gaps=%llu requests=%llu lost=%llu\n",
                u64(tap.messages), u64(tap.adds), u64(tap.trades), u64(tap.deletes),
                u64(tap.traded_qty), u64(stats.datagrams), u64(stats.gaps), u64(stats.requests),
                u64(stats.lost));
}

}  // namespace

int main(int argc, char** argv) {
    static_cast<void>(std::setvbuf(stdout, nullptr, _IOLBF, 0));

    apps::Args args(argc, argv);
    if (args.flag("--help")) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    std::uint16_t stream = 0;
    std::uint64_t count = 0;
    net::Endpoint listen{};
    net::Endpoint retransmit{};
    Tap tap;
    tap.quiet = args.flag("--quiet");
    const bool from_start = args.flag("--from-start");
    if (!args.number<std::uint16_t>("--stream", stream, 0, core::kMaxShards - 1) ||
        !args.endpoint("--listen", listen) || !args.endpoint("--retransmit", retransmit) ||
        !args.number<std::uint64_t>("--count", count, 1, UINT64_MAX) || !args.finish()) {
        return 2;
    }
    if (listen.port == 0) {
        listen = net::Endpoint{apps::kDefaultFeedGroup,
                               static_cast<std::uint16_t>(apps::kDefaultFeedPort + stream)};
    }
    if (from_start && retransmit.port == 0) {
        std::fprintf(stderr, "--from-start: needs --retransmit, which is where the earlier "
                             "messages come from\n");
        return 2;
    }

    feed::ReceiverConfig config;
    config.listen = listen;
    config.stream_id = stream;
    config.retransmit_server = retransmit;
    config.from_start = from_start;
    // Static: the receive path keeps a datagram-sized buffer on the stack
    // already, and there is only ever one of these.
    static feed::Receiver receiver;
    if (const Status status = receiver.init(config); !ok(status)) {
        const std::string_view reason = core::to_string(status);
        std::fprintf(stderr, "planck-mdtap: cannot listen: %.*s\n",
                     static_cast<int>(reason.size()), reason.data());
        if (net::is_multicast(listen.address)) {
            std::fprintf(stderr, "planck-mdtap: if this host cannot join a multicast group, run "
                                 "the venue with --feed 127.0.0.1:PORT and listen there\n");
        }
        return 1;
    }

    // Say so once the socket is open: a script can wait for this line and
    // know that nothing sent from here on will be missed.
    std::printf("listening for stream %u on %s\n", stream, apps::format_endpoint(listen).c_str());

    const std::atomic<bool>& stop = apps::stop_on_signal();
    const timespec nap{.tv_sec = 0, .tv_nsec = 200'000};
    constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;
    std::uint64_t next_report = sys::monotonic_now().raw() + kNanosPerSecond;
    std::uint64_t reported = 0;
    std::uint64_t lost_reported = 0;

    while (!stop.load(std::memory_order_relaxed) && (count == 0 || tap.messages < count)) {
        if (receiver.poll(tap) == 0) {
            // Nothing waiting. This is a viewer, not a trading system, so
            // it sleeps instead of spinning.
            static_cast<void>(::nanosleep(&nap, nullptr));
        }
        const std::uint64_t lost = receiver.stats().lost;
        if (lost != lost_reported) {
            std::printf("LOST %llu messages: books built from this stream are now wrong\n",
                        static_cast<unsigned long long>(lost - lost_reported));
            lost_reported = lost;
        }
        if (tap.quiet) {
            const std::uint64_t at = sys::monotonic_now().raw();
            if (at >= next_report) {
                std::printf("messages=%llu (+%llu) trades=%llu next_sequence=%llu\n",
                            static_cast<unsigned long long>(tap.messages),
                            static_cast<unsigned long long>(tap.messages - reported),
                            static_cast<unsigned long long>(tap.trades),
                            static_cast<unsigned long long>(receiver.next_sequence()));
                reported = tap.messages;
                next_report = at + kNanosPerSecond;
            }
        }
    }
    print_summary(tap, receiver.stats());
    return 0;
}
