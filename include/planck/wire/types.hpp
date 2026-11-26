#ifndef PLANCK_WIRE_TYPES_HPP
#define PLANCK_WIRE_TYPES_HPP

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace planck::wire {

// The planck wire protocol, version 1.
//
// Every message is a fixed-size struct that starts with a 4-byte MsgHeader.
// Integers are little-endian. Fields are laid out so that every one sits at
// its natural alignment with no hidden padding: what is in the struct is
// exactly what is on the wire. Encoding and decoding are a single memcpy.
//
// Reserved bytes are sent as zero and must be zero on receipt, so a later
// version can give them meaning without ambiguity.
static_assert(std::endian::native == std::endian::little,
              "planck wire format is little-endian and does no byte swapping");

inline constexpr std::uint16_t kProtocolVersion = 1;

// Largest message in the protocol. Buffers of this size hold any message.
inline constexpr std::size_t kMaxMessageSize = 48;

enum class MsgType : std::uint8_t {
    None = 0x00,

    // Session layer, both directions unless noted.
    LoginRequest = 0x01,   // client to venue
    LoginAccepted = 0x02,  // venue to client
    LoginRejected = 0x03,  // venue to client
    Heartbeat = 0x04,
    Logout = 0x05,

    // Order entry, client to venue.
    NewOrder = 0x10,
    CancelOrder = 0x11,

    // Order entry, venue to client.
    OrderAccepted = 0x20,
    OrderRejected = 0x21,
    Executed = 0x22,
    Canceled = 0x23,
    CancelRejected = 0x24,

    // Market data, venue to everyone.
    MdAddOrder = 0x40,
    MdExecuted = 0x41,
    MdDeleted = 0x42,
};

// First four bytes of every message.
struct MsgHeader {
    // Size of the whole message in bytes, this header included.
    std::uint16_t length = 0;
    MsgType type = MsgType::None;
    std::uint8_t reserved = 0;
};

static_assert(sizeof(MsgHeader) == 4 && alignof(MsgHeader) == 2);
static_assert(std::has_unique_object_representations_v<MsgHeader>);

// Why an order or a cancel was refused. The first six values match
// book::RejectReason one for one. The rest are raised before the book.
enum class RejectCode : std::uint8_t {
    None = 0,
    InvalidQuantity = 1,
    PriceOutOfBand = 2,
    NotFillable = 3,
    BookFull = 4,
    UnknownOrder = 5,
    UnknownSymbol = 6,
    Malformed = 7,    // a field held a value outside its range
    NotLoggedIn = 8,
    Busy = 9,         // the venue is shedding load, try again
};

inline constexpr std::size_t kRejectCodeCount = 10;

// Why an order left the book without being fully filled.
enum class CancelReason : std::uint8_t {
    UserRequested = 0,
    Unfilled = 1,      // remainder of an IOC or market order
    Disconnected = 2,  // the owning session dropped
};

// Which side of a fill the recipient was on.
enum class Liquidity : std::uint8_t {
    Maker = 0,  // the order was resting
    Taker = 1,  // the order crossed the spread
};

enum class LoginRejectReason : std::uint8_t {
    BadCredentials = 0,
    AlreadyLoggedIn = 1,
    UnsupportedVersion = 2,
    ServerFull = 3,
};

// A struct that can go on the wire as is: starts with a MsgHeader, names its
// own type code, and has no padding bytes that could leak stale memory.
template <typename T>
concept Message = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> &&
                  std::has_unique_object_representations_v<T> &&
                  (sizeof(T) >= sizeof(MsgHeader)) && (sizeof(T) <= kMaxMessageSize) &&
                  requires(T message) {
                      { T::kType } -> std::convertible_to<MsgType>;
                      { message.header } -> std::same_as<MsgHeader&>;
                  };

// Static strings. "Unknown" for a value outside the enum.
[[nodiscard]] std::string_view to_string(MsgType type) noexcept;
[[nodiscard]] std::string_view to_string(RejectCode code) noexcept;

}  // namespace planck::wire

#endif  // PLANCK_WIRE_TYPES_HPP
