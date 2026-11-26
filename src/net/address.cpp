#include "planck/net/address.hpp"

#include <cstddef>

namespace planck::net {

core::Status parse_ipv4(std::string_view text, std::uint32_t& out) noexcept {
    constexpr std::size_t kMaxLength = 15;  // "255.255.255.255"
    if (text.empty() || text.size() > kMaxLength) {
        return core::Status::InvalidArgument;
    }

    std::uint32_t address = 0;
    std::size_t pos = 0;
    for (int part = 0; part < 4; ++part) {
        const std::size_t start = pos;
        std::uint32_t value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            value = (value * 10U) + static_cast<std::uint32_t>(text[pos] - '0');
            if (value > 255U) {
                return core::Status::InvalidArgument;
            }
            ++pos;
        }
        const std::size_t digits = pos - start;
        // No empty parts, and no leading zeros: "01" could be read as octal.
        if (digits == 0 || (digits > 1 && text[start] == '0')) {
            return core::Status::InvalidArgument;
        }
        address = (address << 8) | value;

        const bool last = (part == 3);
        if (last ? (pos != text.size()) : (pos >= text.size() || text[pos] != '.')) {
            return core::Status::InvalidArgument;
        }
        pos += last ? 0U : 1U;
    }

    out = address;
    return core::Status::Ok;
}

}  // namespace planck::net
