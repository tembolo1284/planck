#ifndef PLANCK_BOOK_ORDER_HPP
#define PLANCK_BOOK_ORDER_HPP

#include <cstddef>
#include <cstdint>

#include "planck/core/config.hpp"
#include "planck/core/types.hpp"
#include "planck/mem/arena.hpp"
#include "planck/mem/pool.hpp"

namespace planck::book {

// A resting order. 32 bytes, so two fit in one cache line. Only what matching
// and cancelling read lives here. Session and client order id belong in a
// parallel cold array indexed by the same slot.
//
// next must stay the first member: while a slot is free, the pool keeps its
// free-list link in those bytes. id must not overlap them: OrderStore zeroes
// id on release and reads it on lookup to tell live slots from free ones.
struct Order {
    mem::Index next = mem::kNullIndex;  // newer order at the same price
    mem::Index prev = mem::kNullIndex;  // older order at the same price
    core::Qty qty{};                    // open quantity
    core::Price price{};
    core::OrderId id = core::kNoOrderId;
    std::uint32_t reserved = 0;
    core::SymbolId symbol{};
    core::Side side = core::Side::Buy;
    std::uint8_t reserved2 = 0;
};

static_assert(sizeof(Order) == 32);
static_assert(alignof(Order) == 8);
static_assert(core::kCacheLine % sizeof(Order) == 0);
static_assert(offsetof(Order, next) == 0);
static_assert(offsetof(Order, id) >= sizeof(mem::Index));
static_assert(mem::Poolable<Order> && mem::ArenaStorable<Order>);

// One price. A FIFO queue of orders, oldest at head. 16 bytes, four to a
// cache line. All orders at a level are on the same side.
struct Level {
    mem::Index head = mem::kNullIndex;
    mem::Index tail = mem::kNullIndex;
    // Sum of open quantity. 64-bit because many 32-bit orders can stack up.
    std::uint64_t total = 0;
};

static_assert(sizeof(Level) == 16);
static_assert(core::kCacheLine % sizeof(Level) == 0);
static_assert(mem::ArenaStorable<Level>);

}  // namespace planck::book

#endif  // PLANCK_BOOK_ORDER_HPP
