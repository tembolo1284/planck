#include "planck/sys/memory.hpp"

#include <cerrno>

#include <sys/mman.h>

#include "planck/core/bits.hpp"
#include "planck/core/config.hpp"
#include "planck/core/contract.hpp"
#include "planck/sys/error.hpp"

namespace planck::sys {

namespace {

using core::Status;

constexpr int kProtection = PROT_READ | PROT_WRITE;
constexpr int kBaseFlags = MAP_PRIVATE | MAP_ANONYMOUS;

// Returns nullptr on failure, with errno set by mmap.
std::byte* map_anonymous(std::size_t bytes, int extra_flags) noexcept {
    PLANCK_EXPECT(bytes > 0);
    PLANCK_EXPECT(core::is_aligned(bytes, core::kPageSize));

    void* const address = ::mmap(nullptr, bytes, kProtection, kBaseFlags | extra_flags, -1, 0);
    if (address == MAP_FAILED) {
        return nullptr;
    }
    return static_cast<std::byte*>(address);
}

}  // namespace

Status map_region(std::size_t bytes, HugePages policy, Region& out) noexcept {
    out = Region{};
    if (bytes == 0 || bytes > kMaxRegionBytes) {
        return Status::InvalidArgument;
    }

    if (policy != HugePages::Off) {
        const std::size_t huge_bytes = core::align_up(bytes, core::kHugePageSize);
        std::byte* const data = map_anonymous(huge_bytes, MAP_HUGETLB);
        if (data != nullptr) {
            out = Region{.data = data, .size = huge_bytes, .huge = true};
            return Status::Ok;
        }
        if (policy == HugePages::Require) {
            return status_from_errno(errno);
        }
    }

    const std::size_t small_bytes = core::align_up(bytes, core::kPageSize);
    std::byte* const data = map_anonymous(small_bytes, 0);
    if (data == nullptr) {
        return status_from_errno(errno);
    }
    if (policy == HugePages::Try) {
        // Advisory only. The mapping is fully usable if the kernel declines.
        static_cast<void>(::madvise(data, small_bytes, MADV_HUGEPAGE));
    }
    out = Region{.data = data, .size = small_bytes, .huge = false};
    return Status::Ok;
}

Status unmap_region(Region& region) noexcept {
    if (region.data == nullptr || region.size == 0) {
        return Status::InvalidArgument;
    }
    if (::munmap(region.data, region.size) != 0) {
        return status_from_errno(errno);
    }
    region = Region{};
    return Status::Ok;
}

void prefault(const Region& region) noexcept {
    PLANCK_EXPECT(region.data != nullptr);
    PLANCK_EXPECT(region.size > 0 && region.size <= kMaxRegionBytes);

    const std::size_t step = region.huge ? core::kHugePageSize : core::kPageSize;
    // volatile keeps the compiler from dropping the read-then-write-back. The
    // write is what makes the kernel back the page with real memory.
    volatile std::byte* const bytes = region.data;
    for (std::size_t offset = 0; offset < region.size; offset += step) {
        const std::byte value = bytes[offset];
        bytes[offset] = value;
    }
}

Status lock_all_memory() noexcept {
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        return status_from_errno(errno);
    }
    return Status::Ok;
}

}  // namespace planck::sys
