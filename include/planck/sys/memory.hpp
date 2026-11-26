#ifndef PLANCK_SYS_MEMORY_HPP
#define PLANCK_SYS_MEMORY_HPP

#include <cstddef>
#include <cstdint>

#include "planck/core/status.hpp"

namespace planck::sys {

// Sanity cap on a single mapping: 1 TiB.
inline constexpr std::size_t kMaxRegionBytes = std::size_t{1} << 40;

enum class HugePages : std::uint8_t {
    Off,      // 4 KiB pages
    Try,      // 2 MiB pages if the pool has them, otherwise 4 KiB pages
    Require,  // 2 MiB pages or fail
};

// An anonymous, zero-filled, read-write mapping. Plain data: whoever maps a
// region must unmap it. Regions are created at startup and normally live for
// the whole process.
struct Region {
    std::byte* data = nullptr;
    std::size_t size = 0;
    bool huge = false;
};

// Map at least bytes of memory. The size is rounded up to the page size in
// use. With Try or Require the pages come from the hugetlbfs pool
// (vm.nr_hugepages). If Try falls back to 4 KiB pages, the region is marked
// for transparent huge pages as a best effort. Pages are not touched: call
// prefault() from the thread that will use them.
[[nodiscard]] core::Status map_region(std::size_t bytes, HugePages policy, Region& out) noexcept;

// Release a region and reset it to empty.
[[nodiscard]] core::Status unmap_region(Region& region) noexcept;

// Touch every page so no page fault happens later on the hot path. Contents
// are preserved. Call from the pinned thread that owns the data, so first
// touch places the pages on that thread's NUMA node.
void prefault(const Region& region) noexcept;

// Lock every current and future page into RAM so nothing is swapped out.
// Needs CAP_IPC_LOCK or a large RLIMIT_MEMLOCK, otherwise returns Permission.
[[nodiscard]] core::Status lock_all_memory() noexcept;

}  // namespace planck::sys

#endif  // PLANCK_SYS_MEMORY_HPP
