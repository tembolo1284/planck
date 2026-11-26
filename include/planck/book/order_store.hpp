#ifndef PLANCK_BOOK_ORDER_STORE_HPP
#define PLANCK_BOOK_ORDER_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "planck/book/order.hpp"
#include "planck/core/contract.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"
#include "planck/mem/pool.hpp"

namespace planck::book {

// Owns the order slots of one shard and issues order ids. Every book in the
// shard shares one store.
//
// An order id packs three fields:
//
//   bits 60..63  shard     which matching thread owns the order
//   bits 24..59  sequence  counts up from 1, never reused
//   bits  0..23  slot      index of the order in this store
//
// The sequence makes ids unique for the day. The slot makes lookup a single
// array access: read the slot, compare the id stored there. No hash table.
// An order that never rests gets the reserved slot value kNoSlot.
//
// Not thread safe. One store belongs to one thread.
class OrderStore {
public:
    static constexpr unsigned kSlotBits = 24;
    static constexpr unsigned kSequenceBits = 36;
    static constexpr unsigned kShardBits = 4;

    static constexpr std::uint64_t kSlotMask = (std::uint64_t{1} << kSlotBits) - 1;
    static constexpr std::uint64_t kMaxSequence = (std::uint64_t{1} << kSequenceBits) - 1;
    static constexpr std::uint64_t kMaxShard = (std::uint64_t{1} << kShardBits) - 1;

    // Slot value carried by ids of orders that never rested.
    static constexpr mem::Index kNoSlot = static_cast<mem::Index>(kSlotMask);
    // Largest number of resting orders one store can hold: 16,777,215.
    static constexpr std::size_t kMaxCapacity = kSlotMask;

    OrderStore() noexcept = default;
    OrderStore(const OrderStore&) = delete;
    OrderStore& operator=(const OrderStore&) = delete;

    // Take over storage. Writes every slot, so call from the owning thread.
    [[nodiscard]] core::Status init(std::span<Order> storage, core::ShardId shard) noexcept;

    // Take a slot for an order that may rest. The slot's id is already set.
    // All other fields are stale. Returns kNullIndex if no slot is free or
    // ids have run out.
    [[nodiscard]] mem::Index acquire() noexcept {
        if (pool_.exhausted() || sequence_ > kMaxSequence) [[unlikely]] {
            return mem::kNullIndex;
        }
        const mem::Index slot = pool_.acquire();
        PLANCK_ASSERT(slot < kNoSlot);
        pool_[slot].id = make_id(slot);
        return slot;
    }

    // Issue an id for an order that will never rest (IOC, FOK, market). Uses
    // no slot. Returns kNoOrderId if ids have run out.
    [[nodiscard]] core::OrderId issue_transient() noexcept {
        if (sequence_ > kMaxSequence) [[unlikely]] {
            return core::kNoOrderId;
        }
        return make_id(kNoSlot);
    }

    // Give a slot back. Its id stops resolving immediately.
    void release(mem::Index slot) noexcept {
        PLANCK_EXPECT(slot < pool_.capacity());
        PLANCK_EXPECT(pool_[slot].id != core::kNoOrderId);
        pool_[slot].id = core::kNoOrderId;
        pool_.release(slot);
    }

    // Slot of a live order, or kNullIndex if id is unknown, already gone, or
    // belongs to an order that never rested. Safe on any 64-bit value.
    [[nodiscard]] mem::Index find(core::OrderId id) const noexcept {
        const auto slot = static_cast<mem::Index>(id.raw() & kSlotMask);
        if (slot >= pool_.capacity() || id == core::kNoOrderId) [[unlikely]] {
            return mem::kNullIndex;
        }
        return (pool_[slot].id == id) ? slot : mem::kNullIndex;
    }

    [[nodiscard]] Order& operator[](mem::Index slot) noexcept { return pool_[slot]; }
    [[nodiscard]] const Order& operator[](mem::Index slot) const noexcept { return pool_[slot]; }

    [[nodiscard]] mem::Index capacity() const noexcept { return pool_.capacity(); }
    [[nodiscard]] mem::Index in_use() const noexcept { return pool_.in_use(); }
    [[nodiscard]] bool exhausted() const noexcept { return pool_.exhausted(); }
    [[nodiscard]] core::ShardId shard() const noexcept {
        return core::ShardId{static_cast<std::uint8_t>(shard_bits_ >> (kSlotBits + kSequenceBits))};
    }

    // Shard that issued an id. Lets a router send a cancel to the right
    // thread without any lookup.
    [[nodiscard]] static constexpr core::ShardId shard_of(core::OrderId id) noexcept {
        return core::ShardId{static_cast<std::uint8_t>(id.raw() >> (kSlotBits + kSequenceBits))};
    }

private:
    [[nodiscard]] core::OrderId make_id(mem::Index slot) noexcept {
        PLANCK_EXPECT(slot <= kNoSlot);
        PLANCK_EXPECT(sequence_ >= 1 && sequence_ <= kMaxSequence);
        const std::uint64_t raw = shard_bits_ | (sequence_ << kSlotBits) | slot;
        ++sequence_;
        return core::OrderId{raw};
    }

    mem::IndexPool<Order> pool_;
    std::uint64_t sequence_ = 1;
    std::uint64_t shard_bits_ = 0;
};

static_assert(OrderStore::kSlotBits + OrderStore::kSequenceBits + OrderStore::kShardBits == 64);
static_assert(OrderStore::kMaxShard + 1 >= core::kMaxShards);

}  // namespace planck::book

#endif  // PLANCK_BOOK_ORDER_STORE_HPP
