#ifndef PLANCK_NET_ADDRESS_HPP
#define PLANCK_NET_ADDRESS_HPP

#include <cstdint>
#include <string_view>

#include "planck/core/status.hpp"

namespace planck::net {

// An IPv4 address and port. Both are in host byte order here. Conversion to
// network order happens inside the socket calls and nowhere else.
struct Endpoint {
    std::uint32_t address = 0;
    std::uint16_t port = 0;

    friend constexpr bool operator==(const Endpoint&, const Endpoint&) noexcept = default;
};

[[nodiscard]] constexpr std::uint32_t ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                                           std::uint8_t d) noexcept {
    return (std::uint32_t{a} << 24) | (std::uint32_t{b} << 16) | (std::uint32_t{c} << 8) |
           std::uint32_t{d};
}

inline constexpr std::uint32_t kAnyAddress = 0;
inline constexpr std::uint32_t kLoopback = ipv4(127, 0, 0, 1);

// 224.0.0.0 through 239.255.255.255.
[[nodiscard]] constexpr bool is_multicast(std::uint32_t address) noexcept {
    return (address >> 28) == 0xE;
}

// Parse dotted-quad text such as "239.1.2.3". No names, no shorthand, no
// leading zeros. out is written only on success.
[[nodiscard]] core::Status parse_ipv4(std::string_view text, std::uint32_t& out) noexcept;

}  // namespace planck::net

#endif  // PLANCK_NET_ADDRESS_HPP
