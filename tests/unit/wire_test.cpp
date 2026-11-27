#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <span>
#include <vector>

#include "planck/wire/codec.hpp"

namespace planck::wire {
namespace {

using core::Status;

template <typename T>
class EveryMessage : public ::testing::Test {};

using AllMessages = ::testing::Types<LoginRequest, LoginAccepted, LoginRejected, Heartbeat, Logout,
                                     NewOrder, CancelOrder, OrderAccepted, OrderRejected, Executed,
                                     Canceled, CancelRejected, MdAddOrder, MdExecuted, MdDeleted>;
TYPED_TEST_SUITE(EveryMessage, AllMessages);

TYPED_TEST(EveryMessage, DefaultValueHasACorrectHeader) {
    const TypeParam message{};
    EXPECT_EQ(message.header.type, TypeParam::kType);
    EXPECT_EQ(message.header.length, sizeof(TypeParam));
    EXPECT_EQ(message.header.reserved, 0);
    EXPECT_EQ(message_size(TypeParam::kType), sizeof(TypeParam));
    EXPECT_NE(to_string(TypeParam::kType), "Unknown");
}

TYPED_TEST(EveryMessage, RoundTripsThroughBytes) {
    const TypeParam message{};
    std::array<std::byte, kMaxMessageSize> buffer{};
    ASSERT_EQ(encode(message, buffer), Status::Ok);

    TypeParam decoded{};
    ASSERT_EQ(decode(std::span<const std::byte>{buffer}, decoded), Status::Ok);
    EXPECT_EQ(std::memcmp(&message, &decoded, sizeof(TypeParam)), 0);

    const Frame frame = next_frame(std::span<const std::byte>{buffer}.first(sizeof(TypeParam)));
    EXPECT_EQ(frame.status, Status::Ok);
    EXPECT_EQ(frame.type, TypeParam::kType);
    EXPECT_EQ(frame.bytes.size(), sizeof(TypeParam));
}

TYPED_TEST(EveryMessage, ShortBuffersAreHandled) {
    const TypeParam message{};
    std::array<std::byte, kMaxMessageSize> buffer{};
    ASSERT_EQ(encode(message, buffer), Status::Ok);
    const std::span<const std::byte> bytes{buffer};

    EXPECT_EQ(encode(message, std::span{buffer}.first(sizeof(TypeParam) - 1)), Status::Capacity);
    TypeParam decoded{};
    EXPECT_EQ(decode(bytes.first(sizeof(TypeParam) - 1), decoded), Status::WouldBlock);
    for (std::size_t have = 0; have < sizeof(TypeParam); ++have) {
        EXPECT_EQ(next_frame(bytes.first(have)).status, Status::WouldBlock) << have;
    }
}

TYPED_TEST(EveryMessage, CorruptHeaderIsRejected) {
    const TypeParam message{};
    // Offsets within MsgHeader: 1 is the high byte of length, 2 the type,
    // 3 the reserved byte.
    for (const std::size_t offset : {1U, 2U, 3U}) {
        std::array<std::byte, kMaxMessageSize> buffer{};
        ASSERT_EQ(encode(message, buffer), Status::Ok);
        buffer[offset] = std::byte{0x7E};

        TypeParam decoded{};
        EXPECT_EQ(decode(std::span<const std::byte>{buffer}, decoded), Status::Protocol) << offset;
        EXPECT_EQ(next_frame(buffer).status, Status::Protocol) << offset;
    }
}

TEST(MessageTable, CoversExactlyTheProtocol) {
    std::size_t known = 0;
    std::size_t largest = 0;
    for (int value = 0; value < 256; ++value) {
        const std::size_t size = message_size(static_cast<MsgType>(value));
        if (size != 0) {
            ++known;
            largest = std::max(largest, size);
            EXPECT_GE(size, sizeof(MsgHeader));
        }
    }
    EXPECT_EQ(known, 15U);
    EXPECT_EQ(largest, kMaxMessageSize);
    EXPECT_EQ(message_size(MsgType::None), 0U);
}

// These arrays are the protocol. If one of these tests fails, the wire format
// has changed and every client is broken.
TEST(GoldenBytes, NewOrder) {
    NewOrder order{};
    order.symbol_id = 0x1234;
    order.side = 1;
    order.order_type = 0;
    order.client_order_id = 0x1122334455667788ULL;
    order.price = -2;
    order.qty = 0x0A0B0C0D;
    order.tif = 2;

    const std::array<unsigned char, 32> expected{
        0x20, 0x00, 0x10, 0x00,                          // length 32, type 0x10, reserved
        0x34, 0x12, 0x01, 0x00,                          // symbol, side, order type
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // client order id
        0xFE, 0xFF, 0xFF, 0xFF,                          // price -2
        0x0D, 0x0C, 0x0B, 0x0A,                          // qty
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // tif, reserved
    };
    std::array<std::byte, 32> actual{};
    ASSERT_EQ(encode(order, actual), Status::Ok);
    EXPECT_EQ(std::memcmp(actual.data(), expected.data(), expected.size()), 0);
}

TEST(GoldenBytes, Executed) {
    Executed fill{};
    fill.liquidity = Liquidity::Taker;
    fill.order_id = 1;
    fill.match_id = 2;
    fill.price = 3;
    fill.qty = 4;
    fill.leaves = 5;
    fill.timestamp = 6;

    const std::array<unsigned char, 48> expected{
        0x30, 0, 0x22, 0, 1, 0, 0, 0,   // length 48, type 0x22, reserved, liquidity, reserved
        1,    0, 0,    0, 0, 0, 0, 0,   // order id
        2,    0, 0,    0, 0, 0, 0, 0,   // match id
        3,    0, 0,    0, 4, 0, 0, 0,   // price, qty
        5,    0, 0,    0, 0, 0, 0, 0,   // leaves, reserved
        6,    0, 0,    0, 0, 0, 0, 0,   // timestamp
    };
    std::array<std::byte, 48> actual{};
    ASSERT_EQ(encode(fill, actual), Status::Ok);
    EXPECT_EQ(std::memcmp(actual.data(), expected.data(), expected.size()), 0);
}

TEST(TrustBoundary, NewOrderFieldRanges) {
    NewOrder good{};
    good.side = 1;
    good.order_type = 1;
    good.tif = 2;
    ASSERT_TRUE(well_formed(good));
    EXPECT_EQ(side_of(good), core::Side::Sell);
    EXPECT_EQ(order_type_of(good), core::OrderType::Market);
    EXPECT_EQ(tif_of(good), core::TimeInForce::FillOrKill);

    const auto broken = [&good](auto change) {
        NewOrder copy = good;
        change(copy);
        return !well_formed(copy);
    };
    EXPECT_TRUE(broken([](NewOrder& m) { m.side = 2; }));
    EXPECT_TRUE(broken([](NewOrder& m) { m.side = 255; }));
    EXPECT_TRUE(broken([](NewOrder& m) { m.order_type = 2; }));
    EXPECT_TRUE(broken([](NewOrder& m) { m.tif = 3; }));
    EXPECT_TRUE(broken([](NewOrder& m) { m.reserved[0] = 1; }));
    EXPECT_TRUE(broken([](NewOrder& m) { m.reserved[6] = 1; }));
}

TEST(TrustBoundary, ReservedFieldsMustBeZero) {
    CancelOrder cancel{};
    EXPECT_TRUE(well_formed(cancel));
    cancel.reserved = 1;
    EXPECT_FALSE(well_formed(cancel));

    LoginRequest login{};
    EXPECT_TRUE(well_formed(login));
    login.reserved2 = 9;
    EXPECT_FALSE(well_formed(login));
}

TEST(Names, RejectCodes) {
    EXPECT_EQ(to_string(RejectCode::None), "None");
    EXPECT_EQ(to_string(RejectCode::Busy), "Busy");
    EXPECT_EQ(to_string(static_cast<RejectCode>(77)), "Unknown");
}

template <Message T>
void append(std::vector<std::byte>& stream, const T& message) {
    const std::size_t at = stream.size();
    stream.resize(at + sizeof(T));
    ASSERT_EQ(encode(message, std::span{stream}.subspan(at)), Status::Ok);
}

// A TCP stream delivers bytes in arbitrary pieces at arbitrary alignment.
TEST(Framing, ReassemblesAStreamDeliveredInRandomChunks) {
    constexpr std::uint64_t kMessages = 20'000;
    std::mt19937_64 rng(7);
    // One junk byte in front, so every message starts at an odd address.
    std::vector<std::byte> stream{std::byte{0xAA}};
    for (std::uint64_t i = 0; i < kMessages; ++i) {
        switch (rng() % 4) {
            case 0: {
                NewOrder message{};
                message.client_order_id = i;
                append(stream, message);
                break;
            }
            case 1: {
                Executed message{};
                message.match_id = i;
                append(stream, message);
                break;
            }
            case 2:
                append(stream, Heartbeat{});
                break;
            default: {
                MdDeleted message{};
                message.order_id = i;
                append(stream, message);
            }
        }
    }

    std::size_t delivered = 1;
    std::size_t consumed = 1;
    std::uint64_t index = 0;
    while (consumed < stream.size()) {
        const Frame frame =
            next_frame(std::span<const std::byte>{stream}.subspan(consumed, delivered - consumed));
        if (frame.status == Status::WouldBlock) {
            ASSERT_LT(delivered, stream.size()) << "asked for more bytes than were ever sent";
            delivered = std::min(stream.size(), delivered + 1 + (rng() % 70));
            continue;
        }
        ASSERT_EQ(frame.status, Status::Ok) << "message " << index;
        if (frame.type == MsgType::NewOrder) {
            NewOrder message{};
            ASSERT_EQ(decode(frame.bytes, message), Status::Ok);
            ASSERT_EQ(message.client_order_id, index);
        } else if (frame.type == MsgType::Executed) {
            Executed message{};
            ASSERT_EQ(decode(frame.bytes, message), Status::Ok);
            ASSERT_EQ(message.match_id, index);
            NewOrder wrong{};
            ASSERT_EQ(decode(frame.bytes, wrong), Status::Protocol) << "decoded as the wrong type";
        }
        consumed += frame.bytes.size();
        ++index;
    }
    EXPECT_EQ(index, kMessages);
}

TEST(Framing, RandomBytesNeverCrashAndNeverYieldABadFrame) {
    std::mt19937_64 rng(11);
    std::array<std::byte, 64> junk{};
    std::size_t accepted = 0;
    for (int round = 0; round < 300'000; ++round) {
        for (std::byte& byte : junk) {
            byte = static_cast<std::byte>(rng());
        }
        // Nudge a share of the inputs toward valid headers, or almost
        // nothing would get past the first check.
        if (round % 3 == 0) {
            junk[1] = std::byte{0};
            junk[3] = std::byte{0};
        }
        if (round % 6 == 0) {
            junk[0] = static_cast<std::byte>(message_size(static_cast<MsgType>(junk[2])));
        }

        const std::size_t length = rng() % (junk.size() + 1);
        const Frame frame = next_frame(std::span<const std::byte>{junk.data(), length});
        if (frame.status == Status::Ok) {
            ++accepted;
            ASSERT_EQ(frame.bytes.size(), message_size(frame.type));
            ASSERT_LE(frame.bytes.size(), length);
            ASSERT_EQ(frame.bytes.data(), junk.data());
        } else {
            ASSERT_TRUE(frame.bytes.empty());
        }
    }
    EXPECT_GT(accepted, 0U) << "the fuzzer never produced a valid frame, so it tested nothing";
}

TEST(WireReports, OnlyWhatASessionIsToldOfItsOrdersIsNumbered) {
    // A client counts these to know which report is which. Counting one too
    // many or one too few puts every later number out.
    int numbered = 0;
    for (int type = 0; type < 256; ++type) {
        numbered += is_report(static_cast<MsgType>(type)) ? 1 : 0;
    }
    EXPECT_EQ(numbered, 5);
    EXPECT_TRUE(is_report(MsgType::OrderAccepted));
    EXPECT_TRUE(is_report(MsgType::OrderRejected));
    EXPECT_TRUE(is_report(MsgType::Executed));
    EXPECT_TRUE(is_report(MsgType::Canceled));
    EXPECT_TRUE(is_report(MsgType::CancelRejected));
    EXPECT_FALSE(is_report(MsgType::LoginAccepted));
    EXPECT_FALSE(is_report(MsgType::Heartbeat));
    EXPECT_FALSE(is_report(MsgType::MdExecuted));
}

}  // namespace
}  // namespace planck::wire
