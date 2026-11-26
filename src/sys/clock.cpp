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

// Attempts per paired sample. One clean attempt is enough, and each costs
// well under a microsecond, so a handful is plenty to dodge an interruption.
constexpr int kSampleAttempts = 8;

Nanos read_clock(clockid_t id) noexcept;

ClockSample sample(clockid_t id) noexcept {
    ClockSample best{};
    std::uint64_t narrowest = ~std::uint64_t{0};
    for (int attempt = 0; attempt < kSampleAttempts; ++attempt) {
        const std::uint64_t before = tsc_now();
        const Nanos time = read_clock(id);
        const std::uint64_t after = tsc_now();
        const std::uint64_t width = after - before;
        if (after >= before && width < narrowest) {
            narrowest = width;
            // The kernel read its clock somewhere inside the bracket. The
            // midpoint halves the worst-case error.
            best = ClockSample{.tsc = before + (width / 2), .time = time};
        }
    }
    return best;
}

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

ClockSample sample_monotonic() noexcept {
    return sample(CLOCK_MONOTONIC_RAW);
}

ClockSample sample_wall() noexcept {
    return sample(CLOCK_REALTIME);
}

Status TscClock::calibrate(Nanos window) noexcept {
    if (window < kMinWindow || window > kMaxWindow) {
        return Status::InvalidArgument;
    }

    // Both ends of the window are paired samples, so a pause between reading
    // the kernel clock and reading the counter cannot skew the measured rate.
    const ClockSample start = sample_monotonic();
    std::uint64_t now_ns = start.time.raw();
    std::uint32_t spins = 0;
    while ((now_ns - start.time.raw()) < window.raw() && spins < kMaxCalibrationSpins) {
        core::cpu_relax();
        now_ns = monotonic_now().raw();
        ++spins;
    }
    const ClockSample end = sample_monotonic();

    if (end.time < start.time || (end.time - start.time) < window || end.tsc <= start.tsc) {
        return Status::Io;
    }

    const std::uint64_t elapsed_ns = (end.time - start.time).raw();
    const std::uint64_t elapsed_ticks = end.tsc - start.tsc;
    const detail::U128 mult = (static_cast<detail::U128>(elapsed_ns) << kShift) / elapsed_ticks;
    const detail::U128 rate =
        (static_cast<detail::U128>(elapsed_ticks) * kNanosPerSecond) / elapsed_ns;
    if (mult == 0 || (mult >> 64) != 0 || rate == 0 || (rate >> 64) != 0) {
        return Status::OutOfRange;
    }

    // Anchor to the wall clock at one instant.
    const ClockSample anchor = sample_wall();

    mult_ = static_cast<std::uint64_t>(mult);
    ticks_per_second_ = static_cast<std::uint64_t>(rate);
    base_tsc_ = anchor.tsc;
    base_wall_ = anchor.time;

    PLANCK_ENSURE(calibrated());
    return Status::Ok;
}

void TscClock::reanchor() noexcept {
    PLANCK_EXPECT(calibrated());
    const ClockSample anchor = sample_wall();
    base_tsc_ = anchor.tsc;
    base_wall_ = anchor.time;
}

}  // namespace planck::sys
