#include "planck/book/ladder_bitmap.hpp"

#include <algorithm>

namespace planck::book {

core::Status LadderBitmap::init(std::span<std::uint64_t> words, std::uint32_t bits) noexcept {
    if (bits == 0 || bits > kMaxBits || (bits & kLow) != 0) {
        return core::Status::InvalidArgument;
    }
    if (words.size() != (bits >> kShift)) {
        return core::Status::InvalidArgument;
    }

    std::ranges::fill(words, std::uint64_t{0});
    tier0_ = words.data();
    bits_ = bits;
    tier1_.fill(0);
    tier2_ = 0;

    PLANCK_ENSURE(empty());
    return core::Status::Ok;
}

}  // namespace planck::book
