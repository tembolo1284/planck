#ifndef PLANCK_TESTS_SUPPORT_SHARD_HARNESS_HPP
#define PLANCK_TESTS_SUPPORT_SHARD_HARNESS_HPP

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <utility>

#include "planck/engine/shard.hpp"

namespace planck::test {

// Two symbols with different bands, one of them over negative prices.
inline constexpr std::array<engine::SymbolConfig, 2> kSymbols{{
    {core::SymbolId{3}, core::Price{900}, 256},
    {core::SymbolId{700}, core::Price{-128}, 256},
}};

// A shard small enough to start instantly and to fill up in a test. It does
// not pin, and it yields when idle, so it behaves on a shared machine.
inline engine::ShardConfig small_shard(std::uint32_t orders = 4096, std::uint32_t ring = 1U << 14) {
    engine::ShardConfig config;
    config.shard = core::ShardId{2};
    config.symbols = kSymbols;
    config.order_capacity = orders;
    config.command_ring_capacity = ring;
    config.report_ring_capacity = ring;
    config.market_ring_capacity = ring;
    config.huge_pages = sys::HugePages::Off;
    config.pin = false;
    config.idle = engine::IdlePolicy::Yield;
    return config;
}

// Build commands the way a gateway would: from a wire message that has
// passed the trust boundary.
inline engine::Command new_order(std::uint16_t session, std::uint16_t symbol, core::Side side,
                                 std::int32_t price, std::uint32_t qty,
                                 std::uint64_t client_order_id,
                                 core::TimeInForce tif = core::TimeInForce::Day,
                                 core::OrderType type = core::OrderType::Limit,
                                 std::uint64_t ingress_tsc = 0) {
    wire::NewOrder message{};
    message.symbol_id = symbol;
    message.side = std::to_underlying(side);
    message.order_type = std::to_underlying(type);
    message.tif = std::to_underlying(tif);
    message.price = price;
    message.qty = qty;
    message.client_order_id = client_order_id;
    EXPECT_TRUE(wire::well_formed(message));
    return engine::make_command(message, core::SessionId{session}, ingress_tsc);
}

inline engine::Command cancel_order(std::uint16_t session, std::uint64_t order_id) {
    wire::CancelOrder message{};
    message.order_id = order_id;
    return engine::make_command(message, core::SessionId{session}, 0);
}

inline std::span<const std::byte> payload(const engine::Report& report) {
    return {report.bytes.data(), report.length};
}

inline std::span<const std::byte> payload(const engine::MarketEvent& event) {
    return {event.bytes.data(), event.length};
}

// Wire type of an engine message. Fails the test if the bytes are not one
// complete, valid wire message.
template <typename Envelope>
wire::MsgType type_of(const Envelope& envelope) {
    const wire::Frame frame = wire::next_frame(payload(envelope));
    EXPECT_EQ(frame.status, core::Status::Ok);
    EXPECT_EQ(frame.bytes.size(), payload(envelope).size());
    return frame.type;
}

// Decode an engine message as wire type T. Fails the test if it is not one.
template <wire::Message T, typename Envelope>
T as(const Envelope& envelope) {
    T message{};
    EXPECT_EQ(wire::decode(payload(envelope), message), core::Status::Ok)
        << "expected " << wire::to_string(T::kType);
    return message;
}

}  // namespace planck::test

#endif  // PLANCK_TESTS_SUPPORT_SHARD_HARNESS_HPP
