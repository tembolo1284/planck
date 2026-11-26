#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <span>
#include <string_view>

#include "planck/net/address.hpp"
#include "planck/net/poller.hpp"
#include "planck/net/socket.hpp"

namespace planck::net {
namespace {

using core::Status;

std::span<const std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string_view text_of(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Loopback delivery is fast but not instant. Poll for a little while.
template <typename Ready>
bool eventually(Ready ready) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
        if (ready()) {
            return true;
        }
        ::usleep(100);
    }
    return false;
}

TEST(Address, ParsesDottedQuads) {
    std::uint32_t address = 0;
    ASSERT_EQ(parse_ipv4("127.0.0.1", address), Status::Ok);
    EXPECT_EQ(address, kLoopback);
    ASSERT_EQ(parse_ipv4("239.1.2.3", address), Status::Ok);
    EXPECT_EQ(address, ipv4(239, 1, 2, 3));
    ASSERT_EQ(parse_ipv4("0.0.0.0", address), Status::Ok);
    EXPECT_EQ(address, kAnyAddress);
    ASSERT_EQ(parse_ipv4("255.255.255.255", address), Status::Ok);
    EXPECT_EQ(address, 0xFFFFFFFFU);
}

TEST(Address, RejectsEverythingElse) {
    for (const std::string_view text :
         {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.-4", "a.b.c.d", "1..2.3", "1.2.3.4 ",
          " 1.2.3.4", "01.2.3.4", "1.2.3.", ".1.2.3", "localhost", "1.2.3.4444"}) {
        std::uint32_t address = 42;
        EXPECT_EQ(parse_ipv4(text, address), Status::InvalidArgument) << "'" << text << "'";
        EXPECT_EQ(address, 42U) << "output touched on failure for '" << text << "'";
    }
}

TEST(Address, KnowsMulticast) {
    EXPECT_TRUE(is_multicast(ipv4(224, 0, 0, 1)));
    EXPECT_TRUE(is_multicast(ipv4(239, 255, 255, 255)));
    EXPECT_FALSE(is_multicast(ipv4(223, 255, 255, 255)));
    EXPECT_FALSE(is_multicast(ipv4(240, 0, 0, 0)));
    EXPECT_FALSE(is_multicast(kLoopback));
}

class TcpPair : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(tcp_listen({kLoopback, 0}, listener_), Status::Ok);
        Endpoint where;
        ASSERT_EQ(listener_.local_endpoint(where), Status::Ok);
        ASSERT_NE(where.port, 0) << "kernel should have picked a port";
        ASSERT_EQ(tcp_connect(where, client_), Status::Ok);
        Endpoint peer;
        ASSERT_TRUE(eventually([&] { return tcp_accept(listener_, server_, peer) == Status::Ok; }));
        EXPECT_EQ(peer.address, kLoopback);
    }

    Socket listener_;
    Socket client_;
    Socket server_;
};

TEST_F(TcpPair, NothingWaitingIsWouldBlock) {
    Socket nobody;
    Endpoint peer;
    EXPECT_EQ(tcp_accept(listener_, nobody, peer), Status::WouldBlock);
    EXPECT_FALSE(nobody.valid());

    std::array<std::byte, 16> buffer{};
    EXPECT_EQ(server_.receive(buffer).status, Status::WouldBlock);
}

TEST_F(TcpPair, BytesTravelBothWays) {
    const IoResult sent = client_.send(bytes_of("hello"));
    ASSERT_EQ(sent.status, Status::Ok);
    ASSERT_EQ(sent.bytes, 5U);

    std::array<std::byte, 16> buffer{};
    IoResult got;
    ASSERT_TRUE(eventually([&] {
        got = server_.receive(buffer);
        return got.status == Status::Ok;
    }));
    EXPECT_EQ(text_of({buffer.data(), got.bytes}), "hello");

    ASSERT_EQ(server_.send(bytes_of("world")).status, Status::Ok);
    ASSERT_TRUE(eventually([&] {
        got = client_.receive(buffer);
        return got.status == Status::Ok;
    }));
    EXPECT_EQ(text_of({buffer.data(), got.bytes}), "world");
}

TEST_F(TcpPair, PeerClosingIsReportedAsClosed) {
    client_.close();
    EXPECT_FALSE(client_.valid());
    std::array<std::byte, 16> buffer{};
    EXPECT_TRUE(eventually([&] { return server_.receive(buffer).status == Status::Closed; }));
}

TEST_F(TcpPair, MoveTransfersTheDescriptor) {
    const int fd = client_.fd();
    Socket moved{std::move(client_)};
    EXPECT_FALSE(client_.valid());  // NOLINT(bugprone-use-after-move): that is the point
    EXPECT_EQ(moved.fd(), fd);
    EXPECT_EQ(moved.send(bytes_of("x")).status, Status::Ok);
}

TEST(Tcp, ConnectingToNobodyIsRefused) {
    // Bind a port, learn its number, then close it so nothing is listening.
    Endpoint where;
    {
        Socket listener;
        ASSERT_EQ(tcp_listen({kLoopback, 0}, listener), Status::Ok);
        ASSERT_EQ(listener.local_endpoint(where), Status::Ok);
    }
    Socket client;
    EXPECT_EQ(tcp_connect(where, client), Status::Closed);
    EXPECT_FALSE(client.valid());
}

TEST(Udp, DatagramsArriveWholeWithTheirSender) {
    Socket receiver;
    ASSERT_EQ(udp_open_receiver({kLoopback, 0}, receiver), Status::Ok);
    Endpoint where;
    ASSERT_EQ(receiver.local_endpoint(where), Status::Ok);

    Socket sender;
    ASSERT_EQ(udp_open_sender(where, sender), Status::Ok);
    ASSERT_EQ(sender.send(bytes_of("first")).status, Status::Ok);
    ASSERT_EQ(sender.send(bytes_of("second!")).status, Status::Ok);

    std::array<std::byte, 64> buffer{};
    Endpoint from;
    IoResult got;
    ASSERT_TRUE(eventually([&] {
        got = receiver.receive_from(buffer, from);
        return got.status == Status::Ok;
    }));
    EXPECT_EQ(text_of({buffer.data(), got.bytes}), "first") << "datagrams must not be merged";
    EXPECT_EQ(from.address, kLoopback);

    ASSERT_TRUE(eventually([&] {
        got = receiver.receive_from(buffer, from);
        return got.status == Status::Ok;
    }));
    EXPECT_EQ(text_of({buffer.data(), got.bytes}), "second!");
    EXPECT_EQ(receiver.receive_from(buffer, from).status, Status::WouldBlock);

    // The receiver can answer the sender directly.
    ASSERT_EQ(receiver.send_to(bytes_of("reply"), from).status, Status::Ok);
    ASSERT_TRUE(eventually([&] {
        got = sender.receive(buffer);
        return got.status == Status::Ok;
    }));
    EXPECT_EQ(text_of({buffer.data(), got.bytes}), "reply");
}

TEST(PollerTest, ReportsOnlySocketsWithData) {
    Socket first;
    Socket second;
    ASSERT_EQ(udp_open_receiver({kLoopback, 0}, first), Status::Ok);
    ASSERT_EQ(udp_open_receiver({kLoopback, 0}, second), Status::Ok);
    Endpoint where;
    ASSERT_EQ(second.local_endpoint(where), Status::Ok);

    Poller poller;
    ASSERT_EQ(poller.init(), Status::Ok);
    EXPECT_EQ(poller.init(), Status::Duplicate);
    ASSERT_EQ(poller.add(first, 111), Status::Ok);
    ASSERT_EQ(poller.add(second, 222), Status::Ok);
    EXPECT_EQ(poller.add(second, 333), Status::Duplicate);

    std::array<Event, 8> events{};
    EXPECT_EQ(poller.wait(events, 0), 0U) << "nothing has been sent yet";

    Socket sender;
    ASSERT_EQ(udp_open_sender(where, sender), Status::Ok);
    ASSERT_EQ(sender.send(bytes_of("ping")).status, Status::Ok);
    ASSERT_EQ(poller.wait(events, 1000), 1U);
    EXPECT_EQ(events[0].token, 222U);
    EXPECT_TRUE(events[0].readable);
    EXPECT_FALSE(events[0].closed);

    // Level-triggered: unread data is reported again.
    EXPECT_EQ(poller.wait(events, 0), 1U);

    ASSERT_EQ(poller.remove(second), Status::Ok);
    EXPECT_EQ(poller.wait(events, 0), 0U);
    EXPECT_EQ(poller.remove(second), Status::NotFound);
}

TEST(PollerTest, ReportsAHangUp) {
    Socket listener;
    ASSERT_EQ(tcp_listen({kLoopback, 0}, listener), Status::Ok);
    Endpoint where;
    ASSERT_EQ(listener.local_endpoint(where), Status::Ok);
    Socket client;
    ASSERT_EQ(tcp_connect(where, client), Status::Ok);
    Socket server;
    Endpoint peer;
    ASSERT_TRUE(eventually([&] { return tcp_accept(listener, server, peer) == Status::Ok; }));

    Poller poller;
    ASSERT_EQ(poller.init(), Status::Ok);
    ASSERT_EQ(poller.add(server, 7), Status::Ok);
    client.close();

    std::array<Event, 8> events{};
    ASSERT_EQ(poller.wait(events, 1000), 1U);
    EXPECT_EQ(events[0].token, 7U);
    EXPECT_TRUE(events[0].closed);
}

}  // namespace
}  // namespace planck::net
