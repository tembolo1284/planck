#ifndef PLANCK_CORE_CONFIG_HPP
#define PLANCK_CORE_CONFIG_HPP

#include <cstddef>

namespace planck::core {

// Hardware geometry. Fixed on purpose: std::hardware_destructive_interference_size
// varies with compiler flags, which would make struct layouts unstable.
inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::size_t kPageSize = std::size_t{4} << 10;
inline constexpr std::size_t kHugePageSize = std::size_t{2} << 20;

// Hard upper bounds. Every table sized by these is allocated once at startup.
inline constexpr std::size_t kMaxShards = 16;
inline constexpr std::size_t kMaxSymbols = 4096;
inline constexpr std::size_t kMaxSessions = 1024;

static_assert((kCacheLine & (kCacheLine - 1)) == 0);
static_assert((kPageSize & (kPageSize - 1)) == 0);
static_assert((kHugePageSize & (kHugePageSize - 1)) == 0);
static_assert((kMaxSymbols & (kMaxSymbols - 1)) == 0);
static_assert((kMaxSessions & (kMaxSessions - 1)) == 0);
static_assert(kMaxShards >= 1 && kMaxShards <= 255);

}  // namespace planck::core

#endif  // PLANCK_CORE_CONFIG_HPP
