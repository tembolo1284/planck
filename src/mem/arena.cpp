#include "planck/mem/arena.hpp"

#include <cstdint>

#include "planck/core/bits.hpp"
#include "planck/core/contract.hpp"

namespace planck::mem {

using core::Status;

Arena::~Arena() noexcept {
    release();
}

Arena::Arena(Arena&& other) noexcept : region_(other.region_), used_(other.used_) {
    other.region_ = sys::Region{};
    other.used_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        release();
        region_ = other.region_;
        used_ = other.used_;
        other.region_ = sys::Region{};
        other.used_ = 0;
    }
    return *this;
}

Status Arena::init(std::size_t bytes, sys::HugePages policy) noexcept {
    if (initialised()) {
        return Status::Duplicate;
    }
    const Status status = sys::map_region(bytes, policy, region_);
    used_ = 0;
    PLANCK_ENSURE(core::ok(status) == initialised());
    return status;
}

std::byte* Arena::allocate(std::size_t bytes, std::size_t alignment) noexcept {
    PLANCK_EXPECT(core::is_pow2(alignment) && alignment <= kMaxArenaAlignment);
    PLANCK_EXPECT(used_ <= region_.size);

    if (bytes == 0 || !initialised()) {
        return nullptr;
    }

    // Align the absolute address, not the offset. The mapping itself is only
    // guaranteed to be page aligned.
    const auto base = reinterpret_cast<std::uintptr_t>(region_.data);
    const std::size_t start = core::align_up(base + used_, alignment) - base;
    if (start > region_.size || bytes > (region_.size - start)) {
        return nullptr;
    }
    used_ = start + bytes;

    // Starts the lifetime of a byte array without writing to it, so the pages
    // stay untouched.
    std::byte* const result = new (region_.data + start) std::byte[bytes];
    PLANCK_ENSURE(core::is_aligned(reinterpret_cast<std::uintptr_t>(result), alignment));
    return result;
}

void Arena::prefault() const noexcept {
    PLANCK_EXPECT(initialised());
    sys::prefault(region_);
}

void Arena::release() noexcept {
    if (initialised()) {
        // munmap of a region we mapped ourselves only fails if our own
        // bookkeeping is corrupt. That is not recoverable.
        const Status status = sys::unmap_region(region_);
        PLANCK_VERIFY(core::ok(status));
    }
    used_ = 0;
}

}  // namespace planck::mem
