#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <ostream>
#include <span>
#include <thread>

#include "planck/core/hint.hpp"
#include "planck/mem/arena.hpp"
#include "planck/ring/spsc.hpp"
#include "support/fixtures.hpp"

namespace planck::ring {
namespace {

using core::Status;

// One cache line, with redundancy so a torn or stale read is detectable.
struct Message {
    std::uint64_t sequence;
    std::uint64_t inverted;
    std::array<std::uint64_t, 5> padding;
    std::uint64_t tripled;
};
static_assert(sizeof(Message) == 64);

Message make(std::uint64_t sequence) {
    return Message{sequence, ~sequence, {}, sequence * 3};
}

bool intact(const Message& message, std::uint64_t expected) {
    return message.sequence == expected && message.inverted == ~expected &&
           message.tripled == expected * 3;
}

enum class Mode { PushPop, PushDrain, PushAllDrain };

struct Case {
    const char* name;
    std::size_t capacity;
    Mode mode;
};

void PrintTo(const Case& c, std::ostream* out) {
    *out << c.name;
}

// One producer thread and one consumer thread. Every message must arrive
// exactly once, intact and in order. Run this under ThreadSanitizer too:
//   cmake -B build-tsan -DPLANCK_SANITIZE=thread -DPLANCK_CONTRACTS=checked
class RingStress : public ::testing::TestWithParam<Case> {};

TEST_P(RingStress, DeliversEverythingOnceInOrder) {
    const Case& test_case = GetParam();
    const std::uint64_t total = test::scaled(300'000);

    mem::Arena arena;
    ASSERT_EQ(arena.init(test_case.capacity * sizeof(Message) + 4096, sys::HugePages::Off), Status::Ok);
    SpscRing<Message> ring;
    ASSERT_EQ(ring.init(arena.allocate_array<Message>(test_case.capacity)), Status::Ok);

    std::atomic<bool> start{false};
    std::uint64_t corrupt = 0;
    std::uint64_t received = 0;

    std::thread consumer([&] {
        while (!start.load()) {
            std::this_thread::yield();
        }
        const auto take = [&](const Message& message) noexcept {
            corrupt += intact(message, received) ? 0U : 1U;
            ++received;
        };
        while (received < total) {
            std::size_t got = 0;
            if (test_case.mode == Mode::PushPop) {
                Message message{};
                if (ring.try_pop(message)) {
                    take(message);
                    got = 1;
                }
            } else {
                got = ring.drain(32, take);
            }
            if (got == 0) {
                std::this_thread::yield();
            }
        }
    });

    start.store(true);
    std::array<Message, 5> batch{};
    for (std::uint64_t next = 0; next < total;) {
        bool pushed = false;
        std::uint64_t count = 1;
        if (test_case.mode == Mode::PushAllDrain) {
            count = std::min<std::uint64_t>({1 + (next % 5), total - next, test_case.capacity});
            for (std::uint64_t i = 0; i < count; ++i) {
                batch[i] = make(next + i);
            }
            pushed = ring.try_push_all(std::span<const Message>{batch}.first(count));
        } else {
            pushed = ring.try_push(make(next));
        }
        if (pushed) {
            next += count;
        } else {
            std::this_thread::yield();
        }
    }
    consumer.join();

    EXPECT_EQ(received, total);
    EXPECT_EQ(corrupt, 0U) << "messages arrived out of order or damaged";
    EXPECT_EQ(ring.size_approx(), 0U);
}

INSTANTIATE_TEST_SUITE_P(
    Rings, RingStress,
    ::testing::Values(Case{"TwoSlots_PushPop", 2, Mode::PushPop},
                      Case{"TwoSlots_Batched", 2, Mode::PushAllDrain},
                      Case{"Small_PushPop", 64, Mode::PushPop},
                      Case{"Small_PushDrain", 64, Mode::PushDrain},
                      Case{"Small_Batched", 64, Mode::PushAllDrain},
                      Case{"Large_PushPop", 65536, Mode::PushPop},
                      Case{"Large_PushDrain", 65536, Mode::PushDrain},
                      Case{"Large_Batched", 65536, Mode::PushAllDrain}),
    ::testing::PrintToStringParamName());

}  // namespace
}  // namespace planck::ring
