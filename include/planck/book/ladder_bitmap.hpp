#ifndef PLANCK_BOOK_LADDER_BITMAP_HPP
#define PLANCK_BOOK_LADDER_BITMAP_HPP

#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include "planck/core/bits.hpp"
#include "planck/core/contract.hpp"
#include "planck/core/status.hpp"

namespace planck::book {

// Which price levels on one side of a book have orders. One bit per tick.
//
// Three tiers. A bit in tier 1 says "this 64-bit word of tier 0 is not zero",
// and a bit in tier 2 says the same about a word of tier 1. Finding the
// nearest occupied level is at most three word reads and three bit scans, no
// matter how far away it is. In the common case the next level is in the same
// tier 0 word and it is one read.
//
// Capacity is 64^3 = 262,144 ticks.
class LadderBitmap {
public:
    static constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t kMaxBits = 64U * 64U * 64U;

    LadderBitmap() noexcept = default;

    // Use words as tier 0 storage and clear everything. bits must be a
    // nonzero multiple of 64, at most kMaxBits, with words.size() == bits / 64.
    [[nodiscard]] core::Status init(std::span<std::uint64_t> words, std::uint32_t bits) noexcept;

    void set(std::uint32_t bit) noexcept {
        PLANCK_EXPECT(bit < bits_);
        const std::uint32_t word = bit >> kShift;
        const std::uint32_t group = word >> kShift;
        tier0_[word] |= mask(bit);
        tier1_[group] |= mask(word);
        tier2_ |= mask(group);
    }

    void clear(std::uint32_t bit) noexcept {
        PLANCK_EXPECT(bit < bits_);
        const std::uint32_t word = bit >> kShift;
        const std::uint32_t group = word >> kShift;
        tier0_[word] &= ~mask(bit);
        if (tier0_[word] == 0) {
            tier1_[group] &= ~mask(word);
            if (tier1_[group] == 0) {
                tier2_ &= ~mask(group);
            }
        }
    }

    [[nodiscard]] bool test(std::uint32_t bit) const noexcept {
        PLANCK_EXPECT(bit < bits_);
        return (tier0_[bit >> kShift] & mask(bit)) != 0;
    }

    [[nodiscard]] bool empty() const noexcept { return tier2_ == 0; }
    [[nodiscard]] std::uint32_t size() const noexcept { return bits_; }

    // Lowest set bit at or above bit. kNone if there is none.
    [[nodiscard]] std::uint32_t lowest_from(std::uint32_t bit) const noexcept {
        PLANCK_EXPECT(bit < bits_);
        const std::uint32_t word = bit >> kShift;
        std::uint64_t found = tier0_[word] & (kAllOnes << (bit & kLow));
        if (found != 0) [[likely]] {
            return (word << kShift) | core::lowest_set(found);
        }

        const std::uint32_t group = word >> kShift;
        found = tier1_[group] & ((kAllOnes << (word & kLow)) << 1);
        if (found != 0) {
            return first_in_word((group << kShift) | core::lowest_set(found));
        }

        found = tier2_ & ((kAllOnes << group) << 1);
        if (found == 0) {
            return kNone;
        }
        const std::uint32_t next_group = core::lowest_set(found);
        return first_in_word((next_group << kShift) | core::lowest_set(tier1_[next_group]));
    }

    // Highest set bit at or below bit. kNone if there is none.
    [[nodiscard]] std::uint32_t highest_upto(std::uint32_t bit) const noexcept {
        PLANCK_EXPECT(bit < bits_);
        const std::uint32_t word = bit >> kShift;
        std::uint64_t found = tier0_[word] & (kAllOnes >> (kLow - (bit & kLow)));
        if (found != 0) [[likely]] {
            return (word << kShift) | core::highest_set(found);
        }

        const std::uint32_t group = word >> kShift;
        found = tier1_[group] & (mask(word) - 1);
        if (found != 0) {
            return last_in_word((group << kShift) | core::highest_set(found));
        }

        found = tier2_ & (mask(group) - 1);
        if (found == 0) {
            return kNone;
        }
        const std::uint32_t prev_group = core::highest_set(found);
        return last_in_word((prev_group << kShift) | core::highest_set(tier1_[prev_group]));
    }

private:
    static constexpr std::uint32_t kShift = 6;
    static constexpr std::uint32_t kLow = 63;
    static constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};

    [[nodiscard]] static constexpr std::uint64_t mask(std::uint32_t index) noexcept {
        return std::uint64_t{1} << (index & kLow);
    }

    // word is known to be nonzero because its tier 1 bit is set.
    [[nodiscard]] std::uint32_t first_in_word(std::uint32_t word) const noexcept {
        PLANCK_EXPECT(tier0_[word] != 0);
        return (word << kShift) | core::lowest_set(tier0_[word]);
    }

    [[nodiscard]] std::uint32_t last_in_word(std::uint32_t word) const noexcept {
        PLANCK_EXPECT(tier0_[word] != 0);
        return (word << kShift) | core::highest_set(tier0_[word]);
    }

    std::uint64_t* tier0_ = nullptr;
    std::uint32_t bits_ = 0;
    std::uint64_t tier2_ = 0;
    std::array<std::uint64_t, 64> tier1_{};
};

}  // namespace planck::book

#endif  // PLANCK_BOOK_LADDER_BITMAP_HPP
