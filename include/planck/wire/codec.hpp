#ifndef PLANCK_WIRE_CODEC_HPP
#define PLANCK_WIRE_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "planck/core/contract.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/wire/market_data.hpp"
#include "planck/wire/order_entry.hpp"
#include "planck/wire/types.hpp"

namespace planck::wire {

// Size in bytes of a message of this type. Zero if the type is not part of
// the protocol.
[[nodiscard]] std::size_t message_size(MsgType type) noexcept;

// Result of looking for one whole message at the front of a byte stream.
struct Frame {
    // Ok:         bytes holds exactly one message of the given type.
    // WouldBlock: the message is not all here yet. Read more and call again.
    // Protocol:   the stream is corrupt. Drop the connection.
    core::Status status = core::Status::WouldBlock;
    MsgType type = MsgType::None;
    std::span<const std::byte> bytes{};
};

// Find the next message in a TCP receive buffer. Never reads past the end of
// buffer and is safe on arbitrary bytes. On Ok, the caller consumes
// frame.bytes.size() bytes and calls again.
[[nodiscard]] Frame next_frame(std::span<const std::byte> buffer) noexcept;

// Copy a message out of raw bytes, which may be at any alignment. Checks
// that the bytes are long enough and that the header names type T with the
// right length. Field values are not checked: see well_formed().
template <Message T>
[[nodiscard]] core::Status decode(std::span<const std::byte> bytes, T& out) noexcept {
    if (bytes.size() < sizeof(T)) {
        return core::Status::WouldBlock;
    }
    T message;
    std::memcpy(static_cast<void*>(&message), bytes.data(), sizeof(T));
    if (message.header.type != T::kType || message.header.length != sizeof(T) ||
        message.header.reserved != 0) {
        return core::Status::Protocol;
    }
    out = message;
    return core::Status::Ok;
}

// Copy a message into the front of out. Capacity if it does not fit.
template <Message T>
[[nodiscard]] core::Status encode(const T& message, std::span<std::byte> out) noexcept {
    PLANCK_EXPECT(message.header.type == T::kType);
    PLANCK_EXPECT(message.header.length == sizeof(T) && message.header.reserved == 0);
    if (out.size() < sizeof(T)) {
        return core::Status::Capacity;
    }
    std::memcpy(out.data(), static_cast<const void*>(&message), sizeof(T));
    return core::Status::Ok;
}

// ---- trust boundary ----
//
// decode() proves the bytes have the right shape. well_formed() proves the
// values inside are ones the rest of planck may rely on: enums in range and
// reserved bytes zero. Past this point no code re-checks them.

[[nodiscard]] bool well_formed(const LoginRequest& message) noexcept;
[[nodiscard]] bool well_formed(const NewOrder& message) noexcept;
[[nodiscard]] bool well_formed(const CancelOrder& message) noexcept;

// Typed views of a NewOrder. Call only after well_formed() returned true.
[[nodiscard]] inline core::Side side_of(const NewOrder& message) noexcept {
    PLANCK_EXPECT(core::is_valid(static_cast<core::Side>(message.side)));
    return static_cast<core::Side>(message.side);
}

[[nodiscard]] inline core::OrderType order_type_of(const NewOrder& message) noexcept {
    PLANCK_EXPECT(core::is_valid(static_cast<core::OrderType>(message.order_type)));
    return static_cast<core::OrderType>(message.order_type);
}

[[nodiscard]] inline core::TimeInForce tif_of(const NewOrder& message) noexcept {
    PLANCK_EXPECT(core::is_valid(static_cast<core::TimeInForce>(message.tif)));
    return static_cast<core::TimeInForce>(message.tif);
}

}  // namespace planck::wire

#endif  // PLANCK_WIRE_CODEC_HPP
