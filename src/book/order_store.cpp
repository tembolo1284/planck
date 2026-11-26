#include "planck/book/order_store.hpp"

namespace planck::book {

core::Status OrderStore::init(std::span<Order> storage, core::ShardId shard) noexcept {
    if (storage.empty() || storage.size() > kMaxCapacity) {
        return core::Status::InvalidArgument;
    }
    if (shard.raw() > kMaxShard || shard.raw() >= core::kMaxShards) {
        return core::Status::OutOfRange;
    }
    if (const core::Status status = pool_.init(storage); !core::ok(status)) {
        return status;
    }

    // Arena memory is zero, and the pool only wrote each slot's first four
    // bytes, so every id is already kNoOrderId: all slots read as free.
    PLANCK_ASSERT(storage.front().id == core::kNoOrderId);
    PLANCK_ASSERT(storage.back().id == core::kNoOrderId);

    sequence_ = 1;
    shard_bits_ = static_cast<std::uint64_t>(shard.raw()) << (kSlotBits + kSequenceBits);
    return core::Status::Ok;
}

}  // namespace planck::book
