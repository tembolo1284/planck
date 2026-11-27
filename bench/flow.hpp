#ifndef PLANCK_BENCH_FLOW_HPP
#define PLANCK_BENCH_FLOW_HPP

#include <array>
#include <cstddef>
#include <cstdint>

// Small pieces the benchmarks are built from. They hold no clock and no
// socket, so tests can drive them directly.
namespace planck::bench {

// xorshift64*: fast, repeatable, and good enough to pick prices with.
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept : state_(seed == 0 ? 1 : seed) {}

    constexpr std::uint64_t next() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }
    // A value in [0, bound). bound must not be zero.
    constexpr std::uint32_t below(std::uint32_t bound) noexcept {
        return static_cast<std::uint32_t>((next() >> 32) % bound);
    }

private:
    std::uint64_t state_;
};

// Decides when each message of a fixed-rate load is due.
//
// The schedule is fixed in advance: message k is due at start + k * interval,
// whatever happened to the ones before it. That matters. A load generator
// that waits for one reply before sending the next slows down exactly when
// the server does, and so never sees the queue a real client would have sat
// in. Measuring each reply against the time its request was due, not the
// time it was finally sent, puts that waiting back into the numbers.
class Pacer {
public:
    constexpr Pacer(std::uint64_t start, std::uint64_t interval) noexcept
        : next_(start), interval_(interval) {}

    // If a message is due at `now`, set `intended` to when it was due, move
    // on to the next one and return true. Call again: after a stall several
    // may be due at once.
    [[nodiscard]] constexpr bool due(std::uint64_t now, std::uint64_t& intended) noexcept {
        if (now < next_) {
            return false;
        }
        intended = next_;
        next_ += interval_;
        return true;
    }

private:
    std::uint64_t next_;
    std::uint64_t interval_;
};

// When a request was sent, and when it should have been.
struct SendTimes {
    std::uint64_t sent = 0;
    std::uint64_t intended = 0;
    // Anything the caller wants handed back with the answer.
    std::uint64_t tag = 0;
};

// Remembers send times for requests that have not been answered yet, keyed
// by request id. Ids must be handed out in order, 1, 2, 3 and so on, and at
// most N may be unanswered at once.
template <std::size_t N>
class SendLog {
    static_assert(N != 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    // False if the slot still holds an unanswered request: too many are in
    // flight.
    [[nodiscard]] bool put(std::uint64_t id, SendTimes times) noexcept {
        Entry& entry = entries_[id & (N - 1)];
        if (entry.id != 0) {
            return false;
        }
        entry.id = id;
        entry.times = times;
        ++pending_;
        return true;
    }

    // False if this id is not waiting for an answer: never sent, or
    // answered already.
    [[nodiscard]] bool take(std::uint64_t id, SendTimes& out) noexcept {
        Entry& entry = entries_[id & (N - 1)];
        if (id == 0 || entry.id != id) {
            return false;
        }
        out = entry.times;
        entry.id = 0;
        --pending_;
        return true;
    }

    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }

private:
    struct Entry {
        std::uint64_t id = 0;
        SendTimes times{};
    };
    std::array<Entry, N> entries_{};
    std::size_t pending_ = 0;
};

// Order ids, oldest first. A load generator keeps its resting orders here
// and cancels the oldest when there are too many, so the book it trades
// against stays the same depth for the whole run.
template <std::size_t N>
class Fifo {
    static_assert(N != 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    [[nodiscard]] bool push(std::uint64_t id) noexcept {
        if (size() == N) {
            return false;
        }
        ids_[tail_ & (N - 1)] = id;
        ++tail_;
        return true;
    }
    [[nodiscard]] bool pop(std::uint64_t& out) noexcept {
        if (head_ == tail_) {
            return false;
        }
        out = ids_[head_ & (N - 1)];
        ++head_;
        return true;
    }
    [[nodiscard]] std::size_t size() const noexcept { return static_cast<std::size_t>(tail_ - head_); }

private:
    std::array<std::uint64_t, N> ids_{};
    std::uint64_t head_ = 0;
    std::uint64_t tail_ = 0;
};

}  // namespace planck::bench

#endif  // PLANCK_BENCH_FLOW_HPP
