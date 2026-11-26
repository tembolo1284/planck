#ifndef PLANCK_MEM_ALIGNED_HPP
#define PLANCK_MEM_ALIGNED_HPP

#include <cstddef>

#include "planck/core/config.hpp"

namespace planck::mem {

// Gives a value a cache line to itself, so two of these written by different
// threads never share a line (false sharing). Size is a multiple of
// kCacheLine.
template <typename T>
struct alignas(core::kCacheLine) CacheAligned {
    T value{};
};

static_assert(alignof(CacheAligned<char>) == core::kCacheLine);
static_assert(sizeof(CacheAligned<char>) == core::kCacheLine);
static_assert(sizeof(CacheAligned<std::size_t>) == core::kCacheLine);

}  // namespace planck::mem

#endif  // PLANCK_MEM_ALIGNED_HPP
