#include "planck/wire/codec.hpp"

#include <algorithm>

namespace planck::wire {

namespace {

using core::Status;

template <std::size_t N>
constexpr bool all_zero(const std::array<std::uint8_t, N>& bytes) noexcept {
    return std::ranges::all_of(bytes, [](std::uint8_t byte) { return byte == 0; });
}

}  // namespace

std::size_t message_size(MsgType type) noexcept {
    switch (type) {
        case MsgType::LoginRequest:
            return sizeof(LoginRequest);
        case MsgType::LoginAccepted:
            return sizeof(LoginAccepted);
        case MsgType::LoginRejected:
            return sizeof(LoginRejected);
        case MsgType::Heartbeat:
            return sizeof(Heartbeat);
        case MsgType::Logout:
            return sizeof(Logout);
        case MsgType::NewOrder:
            return sizeof(NewOrder);
        case MsgType::CancelOrder:
            return sizeof(CancelOrder);
        case MsgType::OrderAccepted:
            return sizeof(OrderAccepted);
        case MsgType::OrderRejected:
            return sizeof(OrderRejected);
        case MsgType::Executed:
            return sizeof(Executed);
        case MsgType::Canceled:
            return sizeof(Canceled);
        case MsgType::CancelRejected:
            return sizeof(CancelRejected);
        case MsgType::MdAddOrder:
            return sizeof(MdAddOrder);
        case MsgType::MdExecuted:
            return sizeof(MdExecuted);
        case MsgType::MdDeleted:
            return sizeof(MdDeleted);
        case MsgType::None:
            return 0;
    }
    // A byte off the wire can hold a value that is not an enumerator.
    return 0;
}

Frame next_frame(std::span<const std::byte> buffer) noexcept {
    if (buffer.size() < sizeof(MsgHeader)) {
        return Frame{.status = Status::WouldBlock};
    }

    MsgHeader header;
    std::memcpy(&header, buffer.data(), sizeof(header));

    // Version 1 has no variable-length messages, so the length field must
    // agree exactly with the type. Checking it catches a desynchronised
    // stream on the very first bad header.
    const std::size_t expected = message_size(header.type);
    if (expected == 0 || header.length != expected || header.reserved != 0) {
        return Frame{.status = Status::Protocol, .type = header.type};
    }
    PLANCK_ASSERT(expected >= sizeof(MsgHeader) && expected <= kMaxMessageSize);

    if (buffer.size() < expected) {
        return Frame{.status = Status::WouldBlock, .type = header.type};
    }
    return Frame{.status = Status::Ok, .type = header.type, .bytes = buffer.first(expected)};
}

bool well_formed(const LoginRequest& message) noexcept {
    return message.reserved == 0 && message.reserved2 == 0;
}

bool well_formed(const NewOrder& message) noexcept {
    return core::is_valid(static_cast<core::Side>(message.side)) &&
           core::is_valid(static_cast<core::OrderType>(message.order_type)) &&
           core::is_valid(static_cast<core::TimeInForce>(message.tif)) &&
           all_zero(message.reserved);
}

bool well_formed(const CancelOrder& message) noexcept {
    return message.reserved == 0;
}

}  // namespace planck::wire
