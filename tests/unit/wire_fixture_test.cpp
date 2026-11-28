#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "planck/wire/codec.hpp"

// The wire format as files, one per message, under tests/fixtures/wire/.
//
// The files are the contract with code in other languages. This test proves
// that the C++ structs still produce them, byte for byte. The tests of
// rust/planck-wire read the same files and prove the same of the Rust codec.
// Neither side trusts the other's code, only the bytes.
//
// The values below are repeated in rust/planck-wire/tests/golden.rs. Change
// one and you must change the other and write the files again:
//
//     PLANCK_WRITE_FIXTURES=1 planck_unit_tests --gtest_filter='WireFixtures.*'
//
// A file that changes is a protocol change. See docs/protocol.md.

#ifndef PLANCK_WIRE_FIXTURE_DIR
#error "PLANCK_WIRE_FIXTURE_DIR must name the directory that holds the files"
#endif

namespace planck::wire {
namespace {

using Bytes = std::vector<std::byte>;

template <Message T>
void append(Bytes& out, const T& message) {
    const std::size_t start = out.size();
    out.resize(start + sizeof(T));
    ASSERT_EQ(encode(message, std::span<std::byte>{out}.subspan(start)), core::Status::Ok);
}

void append(Bytes& out, const PacketHeader& header) {
    const std::size_t start = out.size();
    out.resize(start + sizeof(header));
    std::memcpy(out.data() + start, &header, sizeof(header));
}

std::string path_of(const char* name) {
    return std::string{PLANCK_WIRE_FIXTURE_DIR} + "/" + name + ".bin";
}

// Compare bytes with the file of that name. With PLANCK_WRITE_FIXTURES set,
// write the file first, so a deliberate change is one command.
void expect_fixture(const char* name, const Bytes& bytes) {
    const std::string path = path_of(name);
    if (std::getenv("PLANCK_WRITE_FIXTURES") != nullptr) {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(out.good()) << "could not write " << path;
    }
    std::ifstream in{path, std::ios::binary};
    ASSERT_TRUE(in.is_open()) << "missing " << path;
    const std::string held{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    ASSERT_EQ(held.size(), bytes.size()) << name;
    EXPECT_EQ(std::memcmp(held.data(), bytes.data(), bytes.size()), 0)
        << name << " no longer matches its file: the wire format has changed";
}

template <Message T>
void expect_message(const char* name, const T& message) {
    Bytes bytes;
    append(bytes, message);
    expect_fixture(name, bytes);
}

// ---- the values ----

LoginAccepted login_accepted() {
    LoginAccepted message{};
    message.session_id = 0x0102;
    message.idle_timeout_ms = 5000;
    message.epoch = 0x2122232425262728ULL;
    message.next_report = 7;
    message.live_from = 9;
    message.last_client_order_id = 41;
    return message;
}

OrderAccepted order_accepted() {
    OrderAccepted message{};
    message.client_order_id = 42;
    message.order_id = 0x0001000000000007ULL;
    message.timestamp = 1'700'000'000'000'000'001ULL;
    return message;
}

Executed executed() {
    Executed message{};
    message.liquidity = Liquidity::Taker;
    message.order_id = 0x0001000000000007ULL;
    message.match_id = 99;
    message.price = -125;
    message.qty = 300;
    message.leaves = 700;
    message.timestamp = 1'700'000'000'000'000'002ULL;
    return message;
}

Canceled canceled() {
    Canceled message{};
    message.reason = CancelReason::Disconnected;
    message.order_id = 0x0001000000000007ULL;
    message.qty = 700;
    message.timestamp = 1'700'000'000'000'000'003ULL;
    return message;
}

MdAddOrder md_add_order() {
    MdAddOrder message{};
    message.symbol_id = 0x1234;
    message.side = 1;
    message.order_id = 0x0001000000000008ULL;
    message.price = 10'050;
    message.qty = 1000;
    message.timestamp = 1'700'000'000'000'000'004ULL;
    return message;
}

MdExecuted md_executed() {
    MdExecuted message{};
    message.symbol_id = 0x1234;
    message.order_id = 0x0001000000000008ULL;
    message.match_id = 100;
    message.qty = 250;
    message.price = 10'050;
    message.timestamp = 1'700'000'000'000'000'005ULL;
    return message;
}

MdDeleted md_deleted() {
    MdDeleted message{};
    message.symbol_id = 0x1234;
    message.order_id = 0x0001000000000008ULL;
    message.timestamp = 1'700'000'000'000'000'006ULL;
    return message;
}

// ---- session ----

TEST(WireFixtures, LoginRequest) {
    LoginRequest message{};
    message.client_id = 0x01020304;
    message.token = 0x1112131415161718ULL;
    message.epoch = 0x2122232425262728ULL;
    message.next_report = 0x3132333435363738ULL;
    expect_message("login_request", message);
}

TEST(WireFixtures, LoginAccepted) {
    expect_message("login_accepted", login_accepted());
}

TEST(WireFixtures, LoginRejected) {
    LoginRejected message{};
    message.reason = LoginRejectReason::BadSequence;
    expect_message("login_rejected", message);
}

TEST(WireFixtures, HeartbeatAndLogout) {
    expect_message("heartbeat", Heartbeat{});
    expect_message("logout", Logout{});
}

// ---- order entry ----

TEST(WireFixtures, NewOrder) {
    NewOrder message{};
    message.symbol_id = 0x1234;
    message.side = 1;
    message.order_type = 0;
    message.client_order_id = 0x1122334455667788ULL;
    message.price = -2;
    message.qty = 0x0A0B0C0D;
    message.tif = 2;
    expect_message("new_order", message);
}

TEST(WireFixtures, CancelOrder) {
    CancelOrder message{};
    message.order_id = 0x0001000000000007ULL;
    expect_message("cancel_order", message);
}

TEST(WireFixtures, OrderAccepted) {
    expect_message("order_accepted", order_accepted());
}

TEST(WireFixtures, OrderRejected) {
    OrderRejected message{};
    message.reason = RejectCode::PriceOutOfBand;
    message.client_order_id = 43;
    expect_message("order_rejected", message);
}

TEST(WireFixtures, Executed) {
    expect_message("executed", executed());
}

TEST(WireFixtures, Canceled) {
    expect_message("canceled", canceled());
}

TEST(WireFixtures, CancelRejected) {
    CancelRejected message{};
    message.reason = RejectCode::UnknownOrder;
    message.order_id = 0x0001000000000009ULL;
    expect_message("cancel_rejected", message);
}

// What a client reads off one connection: messages back to back with
// nothing between them.
TEST(WireFixtures, OrderEntryStream) {
    Bytes bytes;
    append(bytes, login_accepted());
    append(bytes, order_accepted());
    append(bytes, executed());
    append(bytes, Heartbeat{});
    append(bytes, canceled());
    expect_fixture("order_entry_stream", bytes);
}

// ---- market data ----

TEST(WireFixtures, MarketDataMessages) {
    expect_message("md_add_order", md_add_order());
    expect_message("md_executed", md_executed());
    expect_message("md_deleted", md_deleted());
}

TEST(WireFixtures, MarketDataPacket) {
    Bytes bytes;
    append(bytes, PacketHeader{.sequence = 1000, .count = 3, .stream_id = 2, .reserved = 0});
    append(bytes, md_add_order());
    append(bytes, md_executed());
    append(bytes, md_deleted());
    expect_fixture("md_packet", bytes);
}

TEST(WireFixtures, MarketDataHeartbeat) {
    Bytes bytes;
    append(bytes, PacketHeader{.sequence = 1003, .count = 0, .stream_id = 2, .reserved = 0});
    expect_fixture("md_heartbeat", bytes);
}

}  // namespace
}  // namespace planck::wire
