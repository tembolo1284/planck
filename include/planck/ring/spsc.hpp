#ifndef PLANCK_RING_SPSC_HPP
#define PLANCK_RING_SPSC_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "planck/core/bits.hpp"
#include "planck/core/config.hpp"
#include "planck/core/contract.hpp"
#include "planck/core/status.hpp"

namespace planck::ring {

// Elements are copied in and out bytewise and never destroyed.
template <typename T>
concept RingStorable = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

// Bounded single-producer, single-consumer queue. Exactly one thread may call
// the producer functions and exactly one other thread the consumer functions.
//
// Wait-free: every call finishes in a bounded number of steps, with no locks,
// no system calls and no compare-and-swap loops. A push or pop in the common
// case is one load of the caller's own index, one element copy and one store.
//
// Why it is fast:
//   - The producer's fields and the consumer's fields sit on separate cache
//     lines, so the two threads do not fight over one line.
//   - Each side keeps a private copy of the other side's index and only
//     re-reads the real one when the copy says the queue looks full (or
//     empty). Most calls touch no memory the other thread writes.
//   - Indices count up forever and are masked on use, so there is no wrap
//     logic and no wasted slot. At a billion messages a second a 64-bit index
//     lasts over 500 years.
//
// The ring does not own its storage. Carve it from an Arena.
template <RingStorable T>
class SpscRing {
public:
    static constexpr std::size_t kMinCapacity = 2;
    static constexpr std::size_t kMaxCapacity = std::size_t{1} << 30;

    SpscRing() noexcept = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Attach storage. Its size is the capacity and must be a power of two in
    // [kMinCapacity, kMaxCapacity]. Call once, before either thread starts
    // using the ring.
    [[nodiscard]] core::Status init(std::span<T> storage) noexcept {
        if (producer_.slots != nullptr) {
            return core::Status::Duplicate;
        }
        if (storage.size() < kMinCapacity || storage.size() > kMaxCapacity ||
            !core::is_pow2(storage.size())) {
            return core::Status::InvalidArgument;
        }
        producer_.slots = storage.data();
        producer_.mask = storage.size() - 1;
        consumer_.slots = storage.data();
        consumer_.mask = storage.size() - 1;
        return core::Status::Ok;
    }

    // ---- producer thread only ----

    // Append one element. False if the ring is full.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        PLANCK_EXPECT(producer_.slots != nullptr);
        const std::uint64_t tail = producer_.tail.load(std::memory_order_relaxed);
        if (!has_room(tail, 1)) [[unlikely]] {
            return false;
        }
        producer_.slots[tail & producer_.mask] = value;
        // release: the consumer must see the element before it sees the index.
        producer_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Append every element or none. The consumer sees them all appear at
    // once, with a single index update. False if there is not room for all.
    // values.size() must not exceed capacity().
    [[nodiscard]] bool try_push_all(std::span<const T> values) noexcept {
        PLANCK_EXPECT(producer_.slots != nullptr);
        PLANCK_EXPECT(values.size() <= capacity());
        const std::uint64_t tail = producer_.tail.load(std::memory_order_relaxed);
        if (!has_room(tail, values.size())) [[unlikely]] {
            return false;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            producer_.slots[(tail + i) & producer_.mask] = values[i];
        }
        producer_.tail.store(tail + values.size(), std::memory_order_release);
        return true;
    }

    // ---- consumer thread only ----

    // Remove the oldest element into out. False if the ring is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        PLANCK_EXPECT(consumer_.slots != nullptr);
        const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
        if (available(head, 1) == 0) [[unlikely]] {
            return false;
        }
        out = consumer_.slots[head & consumer_.mask];
        // release: the producer must not reuse the slot before we have read it.
        consumer_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Hand up to max of the queued elements to fn, oldest first, then free
    // them all with a single index update. Returns how many were consumed.
    // fn receives each element by const reference and must not call back
    // into this ring.
    template <typename F>
        requires std::is_nothrow_invocable_v<F&, const T&>
    std::size_t drain(std::size_t max, F&& fn) noexcept {
        PLANCK_EXPECT(consumer_.slots != nullptr);
        const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
        const std::size_t count = std::min(available(head, max), max);
        for (std::size_t i = 0; i < count; ++i) {
            fn(consumer_.slots[(head + i) & consumer_.mask]);
        }
        if (count != 0) {
            consumer_.head.store(head + count, std::memory_order_release);
        }
        return count;
    }

    // ---- any thread ----

    [[nodiscard]] std::size_t capacity() const noexcept { return producer_.mask + 1; }

    // Number of queued elements. Exact only if neither thread is active.
    // Otherwise a snapshot that may already be stale. For monitoring.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::uint64_t head = consumer_.head.load(std::memory_order_acquire);
        const std::uint64_t tail = producer_.tail.load(std::memory_order_acquire);
        return (tail >= head) ? static_cast<std::size_t>(tail - head) : 0;
    }

private:
    // Producer: is there room for count more elements past tail?
    [[nodiscard]] bool has_room(std::uint64_t tail, std::size_t count) noexcept {
        const std::uint64_t cap = producer_.mask + 1;
        PLANCK_EXPECT(count <= cap);
        if ((tail - producer_.cached_head) + count > cap) {
            // Looks full. Take a fresh look at where the consumer really is.
            producer_.cached_head = consumer_.head.load(std::memory_order_acquire);
            PLANCK_ASSERT(tail - producer_.cached_head <= cap);
            return (tail - producer_.cached_head) + count <= cap;
        }
        return true;
    }

    // Consumer: how many elements are ready at head? May be more than want.
    [[nodiscard]] std::size_t available(std::uint64_t head, std::size_t want) noexcept {
        if ((consumer_.cached_tail - head) < want) {
            // Fewer than wanted. Take a fresh look at where the producer really is.
            consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
        }
        PLANCK_ASSERT(consumer_.cached_tail - head <= consumer_.mask + 1);
        return static_cast<std::size_t>(consumer_.cached_tail - head);
    }

    // Written by the producer. tail is also read by the consumer.
    struct alignas(core::kCacheLine) ProducerSide {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t cached_head = 0;
        T* slots = nullptr;
        std::uint64_t mask = 0;
    };

    // Written by the consumer. head is also read by the producer.
    struct alignas(core::kCacheLine) ConsumerSide {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t cached_tail = 0;
        T* slots = nullptr;
        std::uint64_t mask = 0;
    };

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(sizeof(ProducerSide) == core::kCacheLine);
    static_assert(sizeof(ConsumerSide) == core::kCacheLine);

    ProducerSide producer_;
    ConsumerSide consumer_;
};

}  // namespace planck::ring

#endif  // PLANCK_RING_SPSC_HPP
