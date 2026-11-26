#ifndef PLANCK_SYS_CPU_HPP
#define PLANCK_SYS_CPU_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "planck/core/contract.hpp"
#include "planck/core/status.hpp"

namespace planck::sys {

// Matches glibc's CPU_SETSIZE.
inline constexpr std::size_t kMaxCpus = 1024;
// Linux limit for a thread name, excluding the terminator.
inline constexpr std::size_t kMaxThreadName = 15;
// SCHED_FIFO priority range on Linux.
inline constexpr int kMinRealtimePriority = 1;
inline constexpr int kMaxRealtimePriority = 99;

// Fixed-size set of CPU numbers. No allocation.
class CpuSet {
public:
    constexpr void clear() noexcept { words_.fill(0); }

    constexpr void set(std::size_t cpu) noexcept {
        PLANCK_EXPECT(cpu < kMaxCpus);
        words_[cpu / kWordBits] |= (std::uint64_t{1} << (cpu % kWordBits));
    }

    // Adds every cpu in [first, last].
    constexpr void set_range(std::size_t first, std::size_t last) noexcept {
        PLANCK_EXPECT(first <= last);
        PLANCK_EXPECT(last < kMaxCpus);
        for (std::size_t cpu = first; cpu <= last; ++cpu) {
            set(cpu);
        }
    }

    // False for any cpu at or beyond kMaxCpus.
    [[nodiscard]] constexpr bool test(std::size_t cpu) const noexcept {
        if (cpu >= kMaxCpus) {
            return false;
        }
        return ((words_[cpu / kWordBits] >> (cpu % kWordBits)) & std::uint64_t{1}) != 0;
    }

    [[nodiscard]] constexpr std::size_t count() const noexcept {
        std::size_t total = 0;
        for (const std::uint64_t word : words_) {
            total += static_cast<std::size_t>(std::popcount(word));
        }
        PLANCK_ENSURE(total <= kMaxCpus);
        return total;
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return count() == 0; }

    friend constexpr bool operator==(const CpuSet&, const CpuSet&) noexcept = default;

private:
    static constexpr std::size_t kWordBits = 64;
    std::array<std::uint64_t, kMaxCpus / kWordBits> words_{};
};

// Parse the kernel's cpulist format: "2-5,8,10-11", optionally ending in a
// newline. An empty string is the empty set. out is written only on success.
[[nodiscard]] core::Status parse_cpu_list(std::string_view text, CpuSet& out) noexcept;

// CPUs removed from the scheduler with isolcpus=. NotFound if the kernel does
// not expose the file.
[[nodiscard]] core::Status isolated_cpus(CpuSet& out) noexcept;

// CPUs running tickless with nohz_full=. NotFound if the kernel was built
// without CONFIG_NO_HZ_FULL.
[[nodiscard]] core::Status nohz_full_cpus(CpuSet& out) noexcept;

// Number of CPUs currently online. At least 1.
[[nodiscard]] std::size_t online_cpu_count() noexcept;

// CPU the calling thread is running on right now.
[[nodiscard]] std::size_t current_cpu() noexcept;

// Restrict the calling thread to one CPU.
[[nodiscard]] core::Status pin_current_thread(std::size_t cpu) noexcept;

// Name the calling thread, as shown by top -H and perf. At most
// kMaxThreadName characters.
[[nodiscard]] core::Status name_current_thread(std::string_view name) noexcept;

// Move the calling thread to SCHED_FIFO at the given priority. Needs
// CAP_SYS_NICE or a suitable RLIMIT_RTPRIO, otherwise returns Permission.
[[nodiscard]] core::Status set_realtime_priority(int priority) noexcept;

struct HotThreadConfig {
    std::size_t cpu = 0;
    std::string_view name = "planck";
    // 0 leaves the scheduling policy alone.
    int realtime_priority = 0;
    // Fail with Unsupported if cpu is not in the isolcpus= set.
    bool require_isolated = false;
};

struct HotThreadReport {
    bool pinned = false;
    bool isolated = false;
    bool tickless = false;
    bool realtime = false;
};

// One call to set up a latency-critical thread: pin, name, check isolation,
// then optionally go realtime. Call it from the thread itself, before the
// thread touches any memory it will use on the hot path. report says how far
// it got, on success and on failure.
[[nodiscard]] core::Status configure_hot_thread(const HotThreadConfig& config,
                                                HotThreadReport& report) noexcept;

}  // namespace planck::sys

#endif  // PLANCK_SYS_CPU_HPP
