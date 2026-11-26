#include "planck/sys/clock.hpp"

#include <ctime>

#if defined(__x86_64__)
#include <cpuid.h>
#endif

#include "planck/core/hint.hpp"

namespace planck::sys {

namespace {

using core::Nanos;
using core::Status;

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;
// Upper bound on calibration spins. Far more than a one second window needs,
// so hitting it means the monotonic clock is not advancing.
constexpr std::uint32_t kMaxCalibrationSpins = std::uint32_t{1} << 30;

Nanos read_clock(clockid_t id) noexcept {
    timespec ts{};
    const int rc = ::clock_gettime(id, &ts);
    PLANCK_VERIFY(rc == 0);
    PLANCK_VERIFY(ts.tv_sec >= 0 && ts.tv_nsec >= 0);
    return Nanos{(static_cast<std::uint64_t>(ts.tv_sec) * kNanosPerSecond) +
                 static_cast<std::uint64_t>(ts.tv_nsec)};
}

}  // namespace

bool tsc_is_invariant() noexcept {
#if defined(__x86_64__)
    // CPUID leaf 0x80000007, EDX bit 8: invariant TSC.
    constexpr unsigned int kLeaf = 0x80000007U;
    constexpr unsigned int kInvariantBit = 1U << 8;
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(kLeaf, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (edx & kInvariantBit) != 0;
#else
    // The AArch64 generic timer is architecturally constant-rate.
    return true;
#endif
}

Nanos wall_now() noexcept {
    return read_clock(CLOCK_REALTIME);
}

Nanos monotonic_now() noexcept {
    return read_clock(CLOCK_MONOTONIC_RAW);
}

Status TscClock::calibrate(Nanos window) noexcept {
    if (window < kMinWindow || window > kMaxWindow) {
        return Status::InvalidArgument;
    }

    const std::uint64_t start_ns = monotonic_now().raw();
    const std::uint64_t start_tsc = tsc_now();
    std::uint64_t end_ns = start_ns;
    std::uint32_t spins = 0;
    while ((end_ns - start_ns) < window.raw() && spins < kMaxCalibrationSpins) {
        core::cpu_relax();
        end_ns = monotonic_now().raw();
        ++spins;
    }
    const std::uint64_t end_tsc = tsc_now();

    if ((end_ns - start_ns) < window.raw() || end_tsc <= start_tsc) {
        return Status::Io;
    }

    const std::uint64_t elapsed_ns = end_ns - start_ns;
    const std::uint64_t elapsed_ticks = end_tsc - start_tsc;
    const detail::U128 mult = (static_cast<detail::U128>(elapsed_ns) << kShift) / elapsed_ticks;
    const detail::U128 rate =
        (static_cast<detail::U128>(elapsed_ticks) * kNanosPerSecond) / elapsed_ns;
    if (mult == 0 || (mult >> 64) != 0 || rate == 0 || (rate >> 64) != 0) {
        return Status::OutOfRange;
    }

    // Anchor to the wall clock. Read the counter between two wall readings
    // and use their midpoint, which halves the error from the read latency.
    const std::uint64_t wall_before = wall_now().raw();
    const std::uint64_t anchor_tsc = tsc_now();
    const std::uint64_t wall_after = wall_now().raw();
    if (wall_after < wall_before) {
        return Status::Io;
    }

    mult_ = static_cast<std::uint64_t>(mult);
    ticks_per_second_ = static_cast<std::uint64_t>(rate);
    base_tsc_ = anchor_tsc;
    base_wall_ = Nanos{wall_before + ((wall_after - wall_before) / 2)};

    PLANCK_ENSURE(calibrated());
    return Status::Ok;
}

}  // namespace planck::sys
