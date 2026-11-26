#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <unistd.h>

#include "planck/engine/shard.hpp"
#include "planck/feed/publisher.hpp"
#include "planck/feed/receiver.hpp"
#include "support/feed_auditor.hpp"
#include "support/fixtures.hpp"
#include "support/shard_harness.hpp"

namespace planck::feed {
namespace {

using core::Side;
using core::Status;
using test::new_order;

// What a subscriber was handed, in the order it was handed.
struct Delivered {
    std::vector<std::uint64_t> sequences;
    std::vector<wire::MsgType> types;

    void operator()(std::uint64_t sequence, std::span<const std::byte> message) noexcept {
        sequences.push_back(sequence);
        types.push_back(wire::next_frame(message).type);
    }

    // True if the sequences are exactly first, first+1, ... with none
    // missing, repeated or out of order.
    [[nodiscard]] bool contiguous_from(std::uint64_t first) const {
        for (std::size_t i = 0; i < sequences.size(); ++i) {
            if (sequences[i] != first + i) {
                return false;
            }
        }
        return true;
    }
};

// One shard and a publisher on the test's own thread. The stream is sent to
// `tap_`, a plain UDP socket the test reads itself. That puts the test in
// the position of the network: it decides which datagrams a Receiver gets to
// see, which is how packet loss is simulated without any test hooks in the
// production code.
class FeedTest : public ::testing::Test {
protected:
    void SetUp() override { start({}); }

    void start(PublisherConfig config) {
        shard_ = std::make_unique<engine::Shard>();
        ASSERT_EQ(shard_->init(test::small_shard()), Status::Ok);
        pointers_ = {shard_.get()};

        tap_ = net::Socket{};
        ASSERT_EQ(net::udp_open_receiver({net::kLoopback, 0}, tap_), Status::Ok);
        ASSERT_EQ(tap_.local_endpoint(tap_endpoint_), Status::Ok);

        destinations_ = {tap_endpoint_};
        config.shards = pointers_;
        config.destinations = destinations_;
        config.retransmit_listen = {net::kLoopback, 0};
        config.idle = engine::IdlePolicy::Yield;
        publisher_ = std::make_unique<Publisher>();
        ASSERT_EQ(publisher_->init(config), Status::Ok);
        ASSERT_EQ(publisher_->retransmit_endpoint(server_), Status::Ok);
        tap_messages_ = 0;
        tap_heartbeats_ = 0;
    }

    // Enter `count` resting orders, which produces `count` MdAddOrder events,
    // and publish them.
    void publish_adds(std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) {
            ASSERT_TRUE(shard_->commands().try_push(
                new_order(1, 3, Side::Buy, 950 - static_cast<std::int32_t>(i % 20), 1, next_id_++)));
        }
        while (shard_->poll() != 0) {
        }
        // One pass publishes a bounded number of events, so keep going until
        // the ring is empty.
        while (publisher_->poll() != 0) {
        }
    }

    // Every datagram the publisher has put on the live stream since the last
    // call. Loopback delivery is quick but not instant, and on a busy machine
    // it can trail the send by milliseconds. So this does not guess how long
    // to wait: it reads until the tap has seen every message and heartbeat
    // the publisher says it has sent.
    std::vector<std::vector<std::byte>> captured() {
        const auto all_here = [this] {
            return tap_messages_ == (publisher_->next_sequence(0) - 1) &&
                   tap_heartbeats_ == publisher_->stats().heartbeats;
        };
        std::vector<std::vector<std::byte>> datagrams;
        std::array<std::byte, wire::kMaxDatagramSize> buffer{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!all_here() && std::chrono::steady_clock::now() < deadline) {
            net::Endpoint from;
            const net::IoResult result = tap_.receive_from(buffer, from);
            if (result.status != Status::Ok) {
                ::usleep(50);
                continue;
            }
            datagrams.emplace_back(buffer.begin(), buffer.begin() + static_cast<long>(result.bytes));
            const wire::PacketHeader header = header_of(datagrams.back());
            tap_messages_ += header.count;
            tap_heartbeats_ += (header.count == 0) ? 1U : 0U;
        }
        EXPECT_TRUE(all_here()) << "the tap saw " << tap_messages_ << " messages and "
                                << tap_heartbeats_ << " heartbeats, fewer than were sent";
        return datagrams;
    }

    static wire::PacketHeader header_of(const std::vector<std::byte>& datagram) {
        wire::PacketHeader header;
        std::memcpy(&header, datagram.data(), sizeof(header));
        return header;
    }

    // A receiver that listens on a port nobody sends to. The test feeds it
    // datagrams by hand, and it talks to the real publisher for recovery.
    void init_receiver(Receiver& receiver, bool from_start = false, bool with_server = true) {
        ReceiverConfig config;
        config.listen = {net::kLoopback, 0};
        config.stream_id = 2;  // small_shard() is shard 2
        config.from_start = from_start;
        if (with_server) {
            config.retransmit_server = server_;
        }
        ASSERT_EQ(receiver.init(config), Status::Ok);
    }

    // Let the receiver and publisher talk until the receiver has caught up
    // to `target` or stops making progress.
    void recover(Receiver& receiver, Delivered& delivered, std::uint64_t target) {
        for (int round = 0; round < 20'000 && receiver.next_sequence() < target; ++round) {
            static_cast<void>(receiver.poll(delivered));
            static_cast<void>(publisher_->poll());
            ::usleep(20);
        }
    }

    std::unique_ptr<engine::Shard> shard_;
    std::array<engine::Shard*, 1> pointers_{};
    std::array<net::Endpoint, 1> destinations_{};
    std::unique_ptr<Publisher> publisher_;
    net::Socket tap_;
    net::Endpoint tap_endpoint_{};
    net::Endpoint server_{};
    std::uint64_t next_id_ = 1;
    // What captured() has read off the tap so far.
    std::uint64_t tap_messages_ = 0;
    std::uint64_t tap_heartbeats_ = 0;
};

TEST(PublisherConfigTest, RejectsBadConfigurations) {
    auto shard = std::make_unique<engine::Shard>();
    ASSERT_EQ(shard->init(test::small_shard()), Status::Ok);
    engine::Shard* const shards[] = {shard.get()};
    const net::Endpoint one[] = {{net::kLoopback, 40000}};
    const net::Endpoint two[] = {{net::kLoopback, 40000}, {net::kLoopback, 40001}};

    const auto init_with = [&](auto change) {
        PublisherConfig config;
        config.shards = shards;
        config.destinations = one;
        change(config);
        Publisher publisher;
        const Status status = publisher.init(config);
        EXPECT_FALSE(publisher.initialised());
        return status;
    };
    EXPECT_EQ(init_with([](PublisherConfig& c) { c.shards = {}; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([&](PublisherConfig& c) { c.destinations = two; }), Status::InvalidArgument)
        << "one destination per shard";
    EXPECT_EQ(init_with([](PublisherConfig& c) { c.history_messages = 1000; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](PublisherConfig& c) { c.history_messages = 16; }), Status::InvalidArgument);
    EXPECT_EQ(init_with([](PublisherConfig& c) { c.heartbeat_interval = core::Nanos{0}; }),
              Status::InvalidArgument);
}

TEST_F(FeedTest, EveryEventGoesOutNumberedFromOne) {
    publish_adds(5);
    const auto datagrams = captured();
    ASSERT_FALSE(datagrams.empty());

    Receiver receiver;
    init_receiver(receiver);
    Delivered delivered;
    for (const auto& datagram : datagrams) {
        static_cast<void>(receiver.ingest(datagram, delivered));
    }
    ASSERT_EQ(delivered.sequences.size(), 5U);
    EXPECT_TRUE(delivered.contiguous_from(1));
    for (const wire::MsgType type : delivered.types) {
        EXPECT_EQ(type, wire::MsgType::MdAddOrder);
    }
    EXPECT_EQ(header_of(datagrams[0]).sequence, 1U);
    EXPECT_EQ(header_of(datagrams[0]).stream_id, 2);
    EXPECT_EQ(publisher_->next_sequence(0), 6U);
    EXPECT_EQ(publisher_->stats().messages, 5U);
}

TEST_F(FeedTest, PacksManyMessagesPerDatagramWithoutSplittingAny) {
    publish_adds(200);  // 200 x 32 bytes will not fit in one 1400-byte datagram
    const auto datagrams = captured();
    ASSERT_GT(datagrams.size(), 1U);
    EXPECT_LT(datagrams.size(), 20U) << "barely batching at all";

    std::uint64_t expected = 1;
    for (const auto& datagram : datagrams) {
        const wire::PacketHeader header = header_of(datagram);
        ASSERT_LE(datagram.size(), wire::kMaxDatagramSize);
        ASSERT_EQ(header.sequence, expected);
        ASSERT_EQ(datagram.size(), sizeof(header) + header.count * sizeof(wire::MdAddOrder))
            << "a datagram must hold whole messages only";
        expected += header.count;
    }
    EXPECT_EQ(expected, 201U);
}

TEST_F(FeedTest, QuietStreamSendsHeartbeats) {
    PublisherConfig config;
    config.heartbeat_interval = core::Nanos{100'000'000};
    start(config);
    publish_adds(3);
    static_cast<void>(captured());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    static_cast<void>(publisher_->poll());
    // An immediate second pass has not been quiet for long enough to send
    // another one.
    static_cast<void>(publisher_->poll());
    EXPECT_EQ(publisher_->stats().heartbeats, 1U);

    const auto datagrams = captured();
    ASSERT_EQ(datagrams.size(), 1U);
    const wire::PacketHeader header = header_of(datagrams[0]);
    EXPECT_EQ(datagrams[0].size(), sizeof(header));
    EXPECT_EQ(header.count, 0);
    EXPECT_EQ(header.sequence, 4U) << "a heartbeat carries the next sequence to be used";
}

TEST_F(FeedTest, DuplicateAndForeignDatagramsAreIgnored) {
    publish_adds(4);
    const auto datagrams = captured();
    ASSERT_EQ(datagrams.size(), 1U);

    Receiver receiver;
    init_receiver(receiver);
    Delivered delivered;
    EXPECT_EQ(receiver.ingest(datagrams[0], delivered), 4U);
    EXPECT_EQ(receiver.ingest(datagrams[0], delivered), 0U) << "delivered a datagram twice";

    auto other_stream = datagrams[0];
    wire::PacketHeader header = header_of(other_stream);
    header.stream_id = 9;
    header.sequence = 5;
    std::memcpy(other_stream.data(), &header, sizeof(header));
    EXPECT_EQ(receiver.ingest(other_stream, delivered), 0U);

    const std::array<std::byte, 5> runt{};
    EXPECT_EQ(receiver.ingest(runt, delivered), 0U);
    EXPECT_EQ(delivered.sequences.size(), 4U);
    EXPECT_FALSE(receiver.recovering());
}

TEST_F(FeedTest, OverlappingRetransmissionDeliversOnlyTheNewPart) {
    publish_adds(10);
    const auto first = captured();
    ASSERT_EQ(first.size(), 1U);
    publish_adds(5);
    static_cast<void>(captured());  // messages 11..15 are "lost" on the live stream

    Receiver receiver;
    init_receiver(receiver);
    Delivered delivered;
    ASSERT_EQ(receiver.ingest(first[0], delivered), 10U);

    // Ask the publisher for 6..15 by hand. Half of that the receiver already
    // has, which is what a real retransmission often looks like.
    net::Socket asker;
    ASSERT_EQ(net::udp_open_receiver({net::kLoopback, 0}, asker), Status::Ok);
    const wire::PacketHeader request{.sequence = 6, .count = 10, .stream_id = 2, .reserved = 0};
    std::array<std::byte, sizeof(request)> bytes{};
    std::memcpy(bytes.data(), &request, sizeof(request));
    ASSERT_EQ(asker.send_to(bytes, server_).status, Status::Ok);

    std::array<std::byte, wire::kMaxDatagramSize> buffer{};
    net::IoResult reply;
    for (int round = 0; round < 5000; ++round) {
        static_cast<void>(publisher_->poll());
        net::Endpoint from;
        reply = asker.receive_from(buffer, from);
        if (reply.status == Status::Ok) {
            break;
        }
        ::usleep(50);
    }
    ASSERT_EQ(reply.status, Status::Ok);

    EXPECT_EQ(receiver.ingest({buffer.data(), reply.bytes}, delivered), 5U)
        << "must deliver 11..15 and skip 6..10";
    EXPECT_EQ(delivered.sequences.size(), 15U);
    EXPECT_TRUE(delivered.contiguous_from(1)) << "a message was delivered twice";
    EXPECT_EQ(receiver.next_sequence(), 16U);
}

TEST_F(FeedTest, RecoversADatagramLostInTheMiddle) {
    publish_adds(40);
    publish_adds(40);
    publish_adds(40);
    const auto datagrams = captured();
    ASSERT_EQ(datagrams.size(), 3U);

    Receiver receiver;
    init_receiver(receiver);
    Delivered delivered;
    EXPECT_EQ(receiver.ingest(datagrams[0], delivered), 40U);
    // The second datagram is "lost". The third reveals the hole.
    EXPECT_EQ(receiver.ingest(datagrams[2], delivered), 0U) << "delivered past a gap";
    EXPECT_TRUE(receiver.recovering());
    EXPECT_EQ(receiver.next_sequence(), 41U);

    recover(receiver, delivered, 121);
    EXPECT_EQ(delivered.sequences.size(), 120U);
    EXPECT_TRUE(delivered.contiguous_from(1)) << "messages out of order, repeated or missing";
    EXPECT_FALSE(receiver.recovering());
    EXPECT_EQ(receiver.stats().gaps, 1U);
    EXPECT_EQ(receiver.stats().lost, 0U);
    EXPECT_GE(publisher_->stats().retransmitted_messages, 80U);
}

TEST_F(FeedTest, HeartbeatRevealsALostFinalDatagram) {
    PublisherConfig config;
    config.heartbeat_interval = core::Nanos{100'000'000};
    start(config);
    publish_adds(10);
    publish_adds(10);
    const auto data = captured();
    ASSERT_EQ(data.size(), 2U);

    Receiver receiver;
    init_receiver(receiver);
    Delivered delivered;
    EXPECT_EQ(receiver.ingest(data[0], delivered), 10U);
    // The last datagram is lost. With no later data, only a heartbeat can
    // tell the subscriber that it is behind.
    EXPECT_FALSE(receiver.recovering());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    static_cast<void>(publisher_->poll());
    const auto heartbeat = captured();
    ASSERT_EQ(heartbeat.size(), 1U);
    EXPECT_EQ(receiver.ingest(heartbeat[0], delivered), 0U);
    EXPECT_TRUE(receiver.recovering());

    recover(receiver, delivered, 21);
    EXPECT_EQ(delivered.sequences.size(), 20U);
    EXPECT_TRUE(delivered.contiguous_from(1));
}

TEST_F(FeedTest, LateSubscriberCanReplayFromTheStart) {
    publish_adds(1200);
    static_cast<void>(captured());  // all of it happened before the subscriber existed
    publish_adds(5);
    const auto live = captured();
    ASSERT_EQ(live.size(), 1U);

    Receiver receiver;
    init_receiver(receiver, true);
    Delivered delivered;
    EXPECT_EQ(receiver.ingest(live[0], delivered), 0U);
    recover(receiver, delivered, 1206);
    EXPECT_EQ(delivered.sequences.size(), 1205U);
    EXPECT_TRUE(delivered.contiguous_from(1));
    EXPECT_GE(receiver.stats().requests, 3U)
        << "one request returns at most " << Publisher::kMaxRetransmitMessages << " messages";
    EXPECT_EQ(receiver.stats().lost, 0U);
}

TEST_F(FeedTest, WithoutAPublisherToAskAGapIsLostAtOnce) {
    publish_adds(10);
    publish_adds(10);
    publish_adds(10);
    const auto datagrams = captured();
    ASSERT_EQ(datagrams.size(), 3U);

    Receiver receiver;
    init_receiver(receiver, false, false);
    Delivered delivered;
    static_cast<void>(receiver.ingest(datagrams[0], delivered));
    static_cast<void>(receiver.ingest(datagrams[2], delivered));
    static_cast<void>(receiver.poll(delivered));

    // Messages 11..20 were never seen. 21..30 were seen but arrived while the
    // hole was open, so they were discarded and are gone too.
    EXPECT_EQ(receiver.stats().lost, 20U);
    EXPECT_FALSE(receiver.recovering());
    EXPECT_EQ(receiver.next_sequence(), 31U);
    EXPECT_EQ(delivered.sequences.size(), 10U);

    // The stream carries on from there.
    publish_adds(4);
    for (const auto& datagram : captured()) {
        static_cast<void>(receiver.ingest(datagram, delivered));
    }
    EXPECT_EQ(delivered.sequences.back(), 34U);
}

TEST_F(FeedTest, GivesUpOnAPublisherThatNeverAnswers) {
    publish_adds(10);
    publish_adds(10);
    const auto datagrams = captured();
    ASSERT_EQ(datagrams.size(), 2U);

    // Point the receiver at a port where nothing is listening.
    net::Endpoint dead;
    {
        net::Socket closed_soon;
        ASSERT_EQ(net::udp_open_receiver({net::kLoopback, 0}, closed_soon), Status::Ok);
        ASSERT_EQ(closed_soon.local_endpoint(dead), Status::Ok);
    }
    server_ = dead;
    Receiver receiver;
    init_receiver(receiver, true);
    Delivered delivered;
    static_cast<void>(receiver.ingest(datagrams[1], delivered));
    ASSERT_TRUE(receiver.recovering());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (receiver.recovering() && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(receiver.poll(delivered));
        ::usleep(200);
    }
    EXPECT_FALSE(receiver.recovering()) << "still waiting on a publisher that is not there";
    EXPECT_EQ(receiver.stats().requests, Receiver::kMaxRequests);
    EXPECT_EQ(receiver.stats().lost, 20U);
    EXPECT_TRUE(delivered.sequences.empty());
}

TEST_F(FeedTest, ListenerThatArrivesLateGetsTheVeryNextDatagram) {
    // Find a free port, then leave it with nobody listening.
    net::Endpoint where;
    {
        net::Socket probe;
        ASSERT_EQ(net::udp_open_receiver({net::kLoopback, 0}, probe), Status::Ok);
        ASSERT_EQ(probe.local_endpoint(where), Status::Ok);
    }
    destinations_ = {where};
    PublisherConfig config;
    config.shards = pointers_;
    config.destinations = destinations_;
    config.retransmit_listen = {net::kLoopback, 0};
    publisher_ = std::make_unique<Publisher>();
    ASSERT_EQ(publisher_->init(config), Status::Ok);

    // Three messages go to nobody. The kernel notes, against the publisher's
    // socket, that the destination refused them.
    for (std::uint32_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(shard_->commands().try_push(new_order(1, 3, Side::Buy, 950, 1, next_id_++)));
    }
    while (shard_->poll() != 0) {
    }
    while (publisher_->poll() != 0) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Now someone listens. The next datagram must reach them, even though
    // the socket has an error waiting from before.
    net::Socket listener;
    ASSERT_EQ(net::udp_open_receiver(where, listener), Status::Ok);
    for (std::uint32_t i = 0; i < 2; ++i) {
        ASSERT_TRUE(shard_->commands().try_push(new_order(1, 3, Side::Buy, 950, 1, next_id_++)));
    }
    while (shard_->poll() != 0) {
    }
    while (publisher_->poll() != 0) {
    }

    std::array<std::byte, wire::kMaxDatagramSize> buffer{};
    net::IoResult got;
    for (int round = 0; round < 20'000; ++round) {
        net::Endpoint from;
        got = listener.receive_from(buffer, from);
        if (got.status == Status::Ok) {
            break;
        }
        ::usleep(100);
    }
    ASSERT_EQ(got.status, Status::Ok) << "the first datagram after the listener arrived was lost";
    wire::PacketHeader header;
    std::memcpy(&header, buffer.data(), sizeof(header));
    EXPECT_EQ(header.sequence, 4U);
    EXPECT_EQ(header.count, 2);
}

TEST_F(FeedTest, RetransmitPortRefusesNonsense) {
    PublisherConfig config;
    config.history_messages = 1024;
    start(config);
    for (int batch = 0; batch < 30; ++batch) {
        publish_adds(100);  // 3000 messages: the first ones fall out of history
        static_cast<void>(captured());
    }

    net::Socket asker;
    ASSERT_EQ(net::udp_open_receiver({net::kLoopback, 0}, asker), Status::Ok);
    const auto ask = [&](const wire::PacketHeader& request) {
        std::array<std::byte, sizeof(request)> bytes{};
        std::memcpy(bytes.data(), &request, sizeof(request));
        ASSERT_EQ(asker.send_to(bytes, server_).status, Status::Ok);
    };
    const std::array<std::byte, 3> runt{};
    ASSERT_EQ(asker.send_to(runt, server_).status, Status::Ok);
    ask({.sequence = 1, .count = 10, .stream_id = 2, .reserved = 0});       // too old
    ask({.sequence = 3001, .count = 10, .stream_id = 2, .reserved = 0});    // not published yet
    ask({.sequence = 2990, .count = 0, .stream_id = 2, .reserved = 0});     // asks for nothing
    ask({.sequence = 2990, .count = 5, .stream_id = 7, .reserved = 0});     // no such stream
    ask({.sequence = 2990, .count = 5, .stream_id = 2, .reserved = 1});     // reserved must be zero
    ask({.sequence = 2990, .count = 5, .stream_id = 2, .reserved = 0});     // fine

    for (int round = 0; round < 2000 && publisher_->stats().retransmit_requests < 7; ++round) {
        static_cast<void>(publisher_->poll());
        ::usleep(50);
    }
    EXPECT_EQ(publisher_->stats().retransmit_requests, 7U);
    EXPECT_EQ(publisher_->stats().retransmit_refused, 6U);
    EXPECT_EQ(publisher_->stats().retransmitted_messages, 5U);

    // The one good request is answered to the asker, in the live format.
    std::array<std::byte, wire::kMaxDatagramSize> buffer{};
    net::Endpoint from;
    net::IoResult reply;
    for (int round = 0; round < 2000; ++round) {
        reply = asker.receive_from(buffer, from);
        if (reply.status == Status::Ok) {
            break;
        }
        ::usleep(50);
    }
    ASSERT_EQ(reply.status, Status::Ok);
    wire::PacketHeader header;
    std::memcpy(&header, buffer.data(), sizeof(header));
    EXPECT_EQ(header.sequence, 2990U);
    EXPECT_EQ(header.count, 5);
    EXPECT_EQ(reply.bytes, sizeof(header) + 5 * sizeof(wire::MdAddOrder));
}

// The whole path over real sockets: random order flow into a shard, its
// market events through the publisher and a UDP socket to a receiver, and a
// FeedAuditor rebuilding the books from what the receiver delivers.
TEST_F(FeedTest, BooksRebuiltOverTheNetworkMatchTheEngine) {
    ReceiverConfig receiver_config;
    receiver_config.listen = {net::kLoopback, 0};
    receiver_config.stream_id = 2;
    receiver_config.retransmit_server = server_;
    Receiver receiver;
    ASSERT_EQ(receiver.init(receiver_config), Status::Ok);
    net::Endpoint where;
    ASSERT_EQ(receiver.local_endpoint(where), Status::Ok);

    // Rebuild the publisher so the stream goes to the receiver's own socket.
    shard_ = std::make_unique<engine::Shard>();
    ASSERT_EQ(shard_->init(test::small_shard(3000, 1U << 12)), Status::Ok);
    pointers_ = {shard_.get()};
    destinations_ = {where};
    PublisherConfig config;
    config.shards = pointers_;
    config.destinations = destinations_;
    config.retransmit_listen = {net::kLoopback, 0};
    publisher_ = std::make_unique<Publisher>();
    ASSERT_EQ(publisher_->init(config), Status::Ok);
    ASSERT_EQ(publisher_->retransmit_endpoint(server_), Status::Ok);

    test::FeedAuditor auditor;
    test::RandomFlow flow(21);
    std::uint64_t delivered = 0;
    const auto on_message = [&](std::uint64_t sequence, std::span<const std::byte> message) noexcept {
        EXPECT_EQ(sequence, delivered + 1);
        ++delivered;
        engine::MarketEvent event{};
        event.length = static_cast<std::uint16_t>(message.size());
        std::memcpy(event.bytes.data(), message.data(), message.size());
        auditor.on_market(event);
    };
    const auto pump = [&] {
        static_cast<void>(shard_->poll());
        shard_->reports().drain(4096, [&](const engine::Report& r) noexcept { auditor.on_report(r); });
        static_cast<void>(publisher_->poll());
        static_cast<void>(receiver.poll(on_message));
    };

    const std::size_t commands = test::scaled(20'000);
    for (std::size_t sent = 0; sent < commands && !::testing::Test::HasFailure(); ++sent) {
        const engine::Command command = flow.next(auditor);
        while (!shard_->commands().try_push(command)) {
            pump();
        }
        if (sent % 5 == 0) {
            pump();
        }
    }
    for (int round = 0; round < 20'000 && delivered + 1 < publisher_->next_sequence(0); ++round) {
        pump();
        ::usleep(20);
    }
    pump();

    EXPECT_EQ(delivered + 1, publisher_->next_sequence(0)) << "the subscriber never caught up";
    EXPECT_EQ(receiver.stats().lost, 0U);
    EXPECT_EQ(auditor.unanswered(), 0U);
    auditor.expect_views_agree();
    auditor.expect_matches(*shard_, test::kSymbols);
    RecordProperty("datagrams", static_cast<int>(publisher_->stats().datagrams));
    RecordProperty("gaps_recovered", static_cast<int>(receiver.stats().gaps));
}

TEST(FeedMulticast, RealGroupOnThisHost) {
    auto shard = std::make_unique<engine::Shard>();
    ASSERT_EQ(shard->init(test::small_shard()), Status::Ok);

    // An administratively scoped group, for use within one site.
    const net::Endpoint group{net::ipv4(239, 255, 42, 99), 47321};
    ReceiverConfig receiver_config;
    receiver_config.listen = group;
    receiver_config.stream_id = 2;
    Receiver receiver;
    if (const Status status = receiver.init(receiver_config); status != Status::Ok) {
        GTEST_SKIP() << "cannot join a multicast group here: " << core::to_string(status);
    }

    engine::Shard* const shards[] = {shard.get()};
    const net::Endpoint destinations[] = {group};
    PublisherConfig config;
    config.shards = shards;
    config.destinations = destinations;
    config.retransmit_listen = {net::kLoopback, 0};
    Publisher publisher;
    if (const Status status = publisher.init(config); status != Status::Ok) {
        GTEST_SKIP() << "cannot send to a multicast group here: " << core::to_string(status);
    }

    for (std::uint64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(shard->commands().try_push(new_order(1, 3, Side::Buy, 950, 1, i)));
    }
    while (shard->poll() != 0) {
    }
    static_cast<void>(publisher.poll());

    Delivered delivered;
    for (int round = 0; round < 5000 && delivered.sequences.size() < 10; ++round) {
        static_cast<void>(receiver.poll(delivered));
        ::usleep(100);
    }
    if (delivered.sequences.empty()) {
        GTEST_SKIP() << "multicast is not routed on this host (sent "
                     << publisher.stats().datagrams << " datagrams, " << publisher.stats().send_failures
                     << " refused)";
    }
    EXPECT_EQ(delivered.sequences.size(), 10U);
    EXPECT_TRUE(delivered.contiguous_from(1));
}

}  // namespace
}  // namespace planck::feed
