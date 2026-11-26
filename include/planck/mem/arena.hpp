#ifndef PLANCK_MEM_ARENA_HPP
#define PLANCK_MEM_ARENA_HPP

#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <type_traits>

#include "planck/core/config.hpp"
#include "planck/core/status.hpp"
#include "planck/sys/memory.hpp"

namespace planck::mem {

// Largest alignment an arena will honour.
inline constexpr std::size_t kMaxArenaAlignment = core::kHugePageSize;

// A type the arena can hand out without running a constructor: its storage is
// all zero bytes, so that must be a valid value, and nothing will ever run
// its destructor.
template <typename T>
concept ArenaStorable = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

// Bump allocator over one mapped region. This is the only source of memory in
// planck: every table, pool and queue is carved out of an arena at startup.
// There is no free and no reset. The mapping is released when the arena is
// destroyed.
//
// Not thread safe. Allocation is a startup activity done by one thread.
class Arena {
public:
    Arena() noexcept = default;
    ~Arena() noexcept;

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;

    // Map the backing region. Duplicate if already initialised.
    [[nodiscard]] core::Status init(std::size_t bytes, sys::HugePages policy) noexcept;

    // Carve out bytes with the given power-of-two alignment. Returns nullptr
    // if the arena is uninitialised or full, or if bytes is zero. The memory
    // is zero-filled and its pages have not been touched.
    [[nodiscard]] std::byte* allocate(std::size_t bytes, std::size_t alignment) noexcept;

    // Carve out an array of count objects, all bytes zero. Returns an empty
    // span on failure. Pages are not touched, so the thread that first writes
    // them decides their NUMA node.
    template <ArenaStorable T>
    [[nodiscard]] std::span<T> allocate_array(std::size_t count) noexcept {
        if (count == 0 || count > (std::numeric_limits<std::size_t>::max() / sizeof(T))) {
            return {};
        }
        std::byte* const raw = allocate(count * sizeof(T), alignof(T));
        if (raw == nullptr) {
            return {};
        }
        // allocate() started the lifetime of a byte array here, which
        // implicitly creates the T objects inside it.
        return std::span<T>{std::launder(reinterpret_cast<T*>(raw)), count};
    }

    // Touch every page of the whole region. Call from the pinned thread that
    // will use the memory.
    void prefault() const noexcept;

    [[nodiscard]] bool initialised() const noexcept { return region_.data != nullptr; }
    [[nodiscard]] bool huge() const noexcept { return region_.huge; }
    [[nodiscard]] std::size_t capacity() const noexcept { return region_.size; }
    [[nodiscard]] std::size_t used() const noexcept { return used_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return region_.size - used_; }

private:
    void release() noexcept;

    sys::Region region_{};
    std::size_t used_ = 0;
};

}  // namespace planck::mem

#endif  // PLANCK_MEM_ARENA_HPP
