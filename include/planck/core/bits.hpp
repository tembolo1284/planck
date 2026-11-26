#ifndef PLANCK_CORE_BITS_HPP
#define PLANCK_CORE_BITS_HPP

#include <bit>
#include <concepts>
#include <cstddef>
#include <limits>

#include "planck/core/contract.hpp"

namespace planck::core {

template <std::unsigned_integral T>
[[nodiscard]] constexpr bool is_pow2(T value) noexcept {
    return std::has_single_bit(value);
}

// Smallest power of two >= value. value must be in [1, 2^63].
[[nodiscard]] constexpr std::size_t ceil_pow2(std::size_t value) noexcept {
    PLANCK_EXPECT(value >= 1);
    PLANCK_EXPECT(value <= (std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1)));
    const std::size_t result = std::bit_ceil(value);
    PLANCK_ENSURE(is_pow2(result));
    return result;
}

// Round value up to a multiple of alignment. alignment must be a power of two.
[[nodiscard]] constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    PLANCK_EXPECT(is_pow2(alignment));
    PLANCK_EXPECT(value <= std::numeric_limits<std::size_t>::max() - (alignment - 1));
    return (value + (alignment - 1)) & ~(alignment - 1);
}

[[nodiscard]] constexpr bool is_aligned(std::size_t value, std::size_t alignment) noexcept {
    PLANCK_EXPECT(is_pow2(alignment));
    return (value & (alignment - 1)) == 0;
}

// Index of the lowest set bit. value must be nonzero.
template <std::unsigned_integral T>
[[nodiscard]] constexpr unsigned lowest_set(T value) noexcept {
    PLANCK_EXPECT(value != 0);
    return static_cast<unsigned>(std::countr_zero(value));
}

// Index of the highest set bit. value must be nonzero.
template <std::unsigned_integral T>
[[nodiscard]] constexpr unsigned highest_set(T value) noexcept {
    PLANCK_EXPECT(value != 0);
    return static_cast<unsigned>(std::numeric_limits<T>::digits - 1 - std::countl_zero(value));
}

}  // namespace planck::core

#endif  // PLANCK_CORE_BITS_HPP
