#ifndef PLANCK_SYS_CLOCK_HPP
#define PLANCK_SYS_CLOCK_HPP

#include <cstdint>

#include "planck/core/contract.hpp"
#include "planck/core/status.hpp"
#include "planck/core/types.hpp"

namespace planck::sys {

namespace detail {
// 128-bit intermediate for tick conversion. __extension__ keeps -Wpedantic quiet.
__extension__ typedef unsigned __int128 U128;
}  // namespace detail

// Raw cycle counter. About 20 cycles and no system call. Use for the start of
// a measured interval and for general timestamping.
[[gnu::always_inline]] inline std::uint64_t tsc_now() noexcept {
#if defined(__x86_64__)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    std::uint64_t ticks = 0;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
#else
#error "planck supports x86-64 and AArch64 only"
#endif
}

// Cycle counter that waits for earlier instructions to finish first. Use for
// the end of a measured interval, so the work being timed is not still in
// flight when the counter is read.
[[gnu::always_inline]] inline std::uint64_t tsc_now_serialized() noexcept {
#if defined(__x86_64__)
    unsigned int aux = 0;
    return __builtin_ia32_rdtscp(&aux);
#elif defined(__aarch64__)
    std::uint64_t ticks = 0;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
#else
#error "planck supports x86-64 and AArch64 only"
#endif
}

// True if the counter ticks at a constant rate regardless of frequency
// scaling and sleep states. If false, TscClock results cannot be trusted.
[[nodiscard]] bool tsc_is_invariant() noexcept;

// CLOCK_REALTIME as nanoseconds since the Unix epoch. Can jump when NTP or
// PTP steps the clock. Not for measuring intervals.
[[nodiscard]] core::Nanos wall_now() noexcept;

// CLOCK_MONOTONIC_RAW in nanoseconds from an arbitrary origin. Never jumps
// and is not slewed.
[[nodiscard]] core::Nanos monotonic_now() noexcept;

// A cycle counter reading and a kernel clock reading that describe the same
// instant.
struct ClockSample {
    std::uint64_t tsc = 0;
    core::Nanos time{};
};

// Read the cycle counter and a kernel clock as one instant. Two separate
// calls cannot do that: the scheduler or a hypervisor can pause the thread
// between them, and the two readings then disagree by however long the pause
// was. These read the counter on both sides of the kernel clock, repeat a
// few times, and keep the attempt with the narrowest bracket.
[[nodiscard]] ClockSample sample_monotonic() noexcept;
[[nodiscard]] ClockSample sample_wall() noexcept;

// Converts cycle counts to nanoseconds. Calibrate once at startup, then copy
// freely: after calibration the object is immutable and safe to read from
// any thread.
class TscClock {
public:
    inline static constexpr core::Nanos kMinWindow{1'000'000};
    inline static constexpr core::Nanos kMaxWindow{1'000'000'000};
    inline static constexpr core::Nanos kDefaultWindow{50'000'000};

    // Measure the counter against CLOCK_MONOTONIC_RAW by spinning for window
    // nanoseconds, then anchor it to the wall clock. A longer window gives a
    // more accurate rate. Run it on the CPU that will do the timing.
    [[nodiscard]] core::Status calibrate(core::Nanos window = kDefaultWindow) noexcept;

    [[nodiscard]] bool calibrated() const noexcept { return mult_ != 0; }

    [[nodiscard]] std::uint64_t ticks_per_second() const noexcept { return ticks_per_second_; }

    // Convert a tick count, normally the difference of two readings.
    [[nodiscard]] core::Nanos to_nanos(std::uint64_t ticks) const noexcept {
        PLANCK_EXPECT(calibrated());
        const detail::U128 product = static_cast<detail::U128>(ticks) * mult_;
        return core::Nanos{static_cast<std::uint64_t>(product >> kShift)};
    }

    // Convert a raw counter reading to nanoseconds since the Unix epoch. The
    // reading must not predate calibration.
    [[nodiscard]] core::Nanos to_wall(std::uint64_t tsc) const noexcept {
        PLANCK_EXPECT(calibrated());
        PLANCK_EXPECT(tsc >= base_tsc_);
        return base_wall_ + to_nanos(tsc - base_tsc_);
    }

    // Wall-clock time without a system call. Drifts from CLOCK_REALTIME by
    // the calibration error, so recalibrate off the hot path if that matters.
    [[nodiscard]] core::Nanos now() const noexcept { return to_wall(tsc_now()); }

private:
    static constexpr unsigned kShift = 32;

    // Nanoseconds per tick in 32.32 fixed point. Zero until calibrated.
    std::uint64_t mult_ = 0;
    std::uint64_t ticks_per_second_ = 0;
    std::uint64_t base_tsc_ = 0;
    core::Nanos base_wall_{};
};

}  // namespace planck::sys

#endif  // PLANCK_SYS_CLOCK_HPP
