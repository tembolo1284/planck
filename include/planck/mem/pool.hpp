#ifndef PLANCK_MEM_POOL_HPP
#define PLANCK_MEM_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

#include "planck/core/contract.hpp"
#include "planck/core/hint.hpp"
#include "planck/core/status.hpp"

namespace planck::mem {

// Handle to a pool slot. Half the size of a pointer, and stable if the pool's
// storage is ever mapped at a different address.
using Index = std::uint32_t;

inline constexpr Index kNullIndex = std::numeric_limits<Index>::max();
inline constexpr std::size_t kMaxPoolCapacity = kNullIndex;

// A free slot stores the index of the next free slot in its own first bytes,
// so T must be big enough to hold one and safe to overwrite bytewise.
template <typename T>
concept Poolable = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> &&
                   sizeof(T) >= sizeof(Index);

// Fixed-capacity object pool addressed by 32-bit index. acquire and release
// are O(1), branch-light and never call the allocator.
//
// A free slot keeps every byte except its first sizeof(Index), so an owner may
// leave a marker further in to tell free slots from live ones.
//
// The pool does not own its storage. Carve it from an Arena. Not thread safe:
// one pool belongs to one thread.
template <Poolable T>
class IndexPool {
public:
    IndexPool() noexcept = default;
    IndexPool(const IndexPool&) = delete;
    IndexPool& operator=(const IndexPool&) = delete;

    // Take over storage and thread the free list through it. This writes to
    // every slot, so it also faults the pages in. Call it from the thread
    // that will own the pool. Duplicate if already initialised.
    [[nodiscard]] core::Status init(std::span<T> storage) noexcept {
        if (slots_ != nullptr) {
            return core::Status::Duplicate;
        }
        if (storage.empty() || storage.size() > kMaxPoolCapacity) {
            return core::Status::InvalidArgument;
        }

        slots_ = storage.data();
        capacity_ = static_cast<Index>(storage.size());
        for (Index index = 0; index < capacity_; ++index) {
            const Index next = index + 1;
            write_next(index, (next < capacity_) ? next : kNullIndex);
        }
        free_head_ = 0;
        in_use_ = 0;
        return core::Status::Ok;
    }

    // Take a slot. Returns kNullIndex when the pool is exhausted. The slot's
    // contents are unspecified: assign the whole object before reading it.
    [[nodiscard]] Index acquire() noexcept {
        PLANCK_EXPECT(slots_ != nullptr);
        if (free_head_ == kNullIndex) [[unlikely]] {
            return kNullIndex;
        }

        const Index index = free_head_;
        free_head_ = read_next(index);
        ++in_use_;
        if (free_head_ != kNullIndex) [[likely]] {
            // Warm the slot the next acquire will return.
            core::prefetch_write(slots_ + free_head_);
        }

        PLANCK_ENSURE(index < capacity_);
        PLANCK_ENSURE(in_use_ <= capacity_);
        return index;
    }

    // Give a slot back. index must have come from acquire() on this pool and
    // must not be released twice.
    void release(Index index) noexcept {
        PLANCK_EXPECT(index < capacity_);
        PLANCK_EXPECT(in_use_ > 0);

        write_next(index, free_head_);
        free_head_ = index;
        --in_use_;
    }

    [[nodiscard]] T& operator[](Index index) noexcept {
        PLANCK_EXPECT(index < capacity_);
        return slots_[index];
    }

    [[nodiscard]] const T& operator[](Index index) const noexcept {
        PLANCK_EXPECT(index < capacity_);
        return slots_[index];
    }

    // Index of an object that lives in this pool.
    [[nodiscard]] Index index_of(const T& object) const noexcept {
        PLANCK_EXPECT(slots_ != nullptr);
        PLANCK_EXPECT(&object >= slots_ && &object < (slots_ + capacity_));
        return static_cast<Index>(&object - slots_);
    }

    [[nodiscard]] Index capacity() const noexcept { return capacity_; }
    [[nodiscard]] Index in_use() const noexcept { return in_use_; }
    [[nodiscard]] Index available() const noexcept { return capacity_ - in_use_; }
    [[nodiscard]] bool exhausted() const noexcept { return free_head_ == kNullIndex; }

private:
    [[nodiscard]] Index read_next(Index index) const noexcept {
        PLANCK_EXPECT(index < capacity_);
        Index next = kNullIndex;
        std::memcpy(&next, static_cast<const void*>(slots_ + index), sizeof(next));
        PLANCK_ENSURE(next < capacity_ || next == kNullIndex);
        return next;
    }

    void write_next(Index index, Index next) noexcept {
        PLANCK_EXPECT(index < capacity_);
        PLANCK_EXPECT(next < capacity_ || next == kNullIndex);
        // The void* cast tells the compiler the bytewise write is intended.
        std::memcpy(static_cast<void*>(slots_ + index), &next, sizeof(next));
    }

    T* slots_ = nullptr;
    Index capacity_ = 0;
    Index free_head_ = kNullIndex;
    Index in_use_ = 0;
};

}  // namespace planck::mem

#endif  // PLANCK_MEM_POOL_HPP
