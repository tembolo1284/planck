#ifndef PLANCK_CORE_STRONG_HPP
#define PLANCK_CORE_STRONG_HPP

#include <compare>
#include <concepts>
#include <limits>
#include <type_traits>

#include "planck/core/contract.hpp"

namespace planck::core {

// A tag opts in to + and - by declaring: static constexpr bool arithmetic = true;
template <typename Tag>
concept ArithmeticTag = requires { requires Tag::arithmetic; };

// Zero-cost distinct integer type. Strong<PriceTag, int32_t> and
// Strong<QtyTag, uint32_t> cannot be mixed up or converted implicitly.
// Same size, alignment and codegen as the underlying integer.
template <typename Tag, std::integral Rep>
class Strong {
public:
    using rep_type = Rep;

    constexpr Strong() noexcept = default;
    constexpr explicit Strong(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep raw() const noexcept { return value_; }

    [[nodiscard]] static constexpr Strong min() noexcept {
        return Strong{std::numeric_limits<Rep>::min()};
    }
    [[nodiscard]] static constexpr Strong max() noexcept {
        return Strong{std::numeric_limits<Rep>::max()};
    }

    friend constexpr bool operator==(Strong, Strong) noexcept = default;
    friend constexpr auto operator<=>(Strong, Strong) noexcept = default;

    friend constexpr Strong operator+(Strong lhs, Strong rhs) noexcept
        requires ArithmeticTag<Tag>
    {
        PLANCK_EXPECT(rhs.value_ <= 0 || lhs.value_ <= std::numeric_limits<Rep>::max() - rhs.value_);
        PLANCK_EXPECT(rhs.value_ >= 0 || lhs.value_ >= std::numeric_limits<Rep>::min() - rhs.value_);
        return Strong{static_cast<Rep>(lhs.value_ + rhs.value_)};
    }

    friend constexpr Strong operator-(Strong lhs, Strong rhs) noexcept
        requires ArithmeticTag<Tag>
    {
        PLANCK_EXPECT(rhs.value_ >= 0 || lhs.value_ <= std::numeric_limits<Rep>::max() + rhs.value_);
        PLANCK_EXPECT(rhs.value_ <= 0 || lhs.value_ >= std::numeric_limits<Rep>::min() + rhs.value_);
        return Strong{static_cast<Rep>(lhs.value_ - rhs.value_)};
    }

    constexpr Strong& operator+=(Strong rhs) noexcept
        requires ArithmeticTag<Tag>
    {
        *this = *this + rhs;
        return *this;
    }

    constexpr Strong& operator-=(Strong rhs) noexcept
        requires ArithmeticTag<Tag>
    {
        *this = *this - rhs;
        return *this;
    }

private:
    Rep value_{};
};

template <typename T>
concept StrongType = requires { typename T::rep_type; } && std::is_trivially_copyable_v<T> &&
                     std::is_standard_layout_v<T> && sizeof(T) == sizeof(typename T::rep_type);

}  // namespace planck::core

#endif  // PLANCK_CORE_STRONG_HPP
