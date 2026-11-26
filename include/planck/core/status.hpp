#ifndef PLANCK_CORE_STATUS_HPP
#define PLANCK_CORE_STATUS_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace planck::core {

// Result code for every fallible operation. No exceptions anywhere in planck.
// [[nodiscard]] on the type makes ignoring one a compile error under -Werror
// (NASA Power of Ten, rule 7).
enum class [[nodiscard]] Status : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    OutOfRange,
    Capacity,
    NotFound,
    Duplicate,
    WouldBlock,
    Closed,
    Io,
    Protocol,
    Unsupported,
};

inline constexpr std::size_t kStatusCount = 11;

[[nodiscard]] constexpr bool ok(Status status) noexcept {
    return status == Status::Ok;
}

// Static string, valid for the life of the process. Returns "Unknown" for a
// value outside the enum.
[[nodiscard]] std::string_view to_string(Status status) noexcept;

}  // namespace planck::core

#endif  // PLANCK_CORE_STATUS_HPP
