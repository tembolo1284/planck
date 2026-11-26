#include "planck/sys/cpu.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <span>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "planck/sys/error.hpp"

namespace planck::sys {

namespace {

using core::ok;
using core::Status;

constexpr std::size_t kMaxCpuListText = 4096;
constexpr int kMaxReadCalls = 16;

constexpr const char* kIsolatedPath = "/sys/devices/system/cpu/isolated";
constexpr const char* kNohzFullPath = "/sys/devices/system/cpu/nohz_full";

constexpr bool is_digit(char ch) noexcept {
    return ch >= '0' && ch <= '9';
}

// Parse a decimal cpu number starting at text[pos]. Advances pos past it.
Status parse_cpu_number(std::string_view text, std::size_t& pos, std::size_t& out) noexcept {
    PLANCK_EXPECT(pos <= text.size());
    PLANCK_EXPECT(text.size() <= kMaxCpuListText);

    const std::size_t start = pos;
    std::size_t value = 0;
    while (pos < text.size() && is_digit(text[pos])) {
        value = (value * 10U) + static_cast<std::size_t>(text[pos] - '0');
        if (value >= kMaxCpus) {
            return Status::OutOfRange;
        }
        ++pos;
    }
    if (pos == start) {
        return Status::Protocol;
    }
    out = value;
    return Status::Ok;
}

// Read a whole small file into buffer. Capacity if it does not fit.
Status read_small_file(const char* path, std::span<char> buffer, std::size_t& length) noexcept {
    PLANCK_EXPECT(path != nullptr);
    PLANCK_EXPECT(!buffer.empty());

    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return status_from_errno(errno);
    }

    std::size_t total = 0;
    bool reached_end = false;
    Status result = Status::Ok;
    for (int call = 0; call < kMaxReadCalls && total < buffer.size(); ++call) {
        const ssize_t got = ::read(fd, buffer.data() + total, buffer.size() - total);
        if (got > 0) {
            total += static_cast<std::size_t>(got);
        } else if (got == 0) {
            reached_end = true;
            break;
        } else if (errno != EINTR) {
            result = status_from_errno(errno);
            break;
        }
    }

    // Nothing was written, so a failed close cannot lose data.
    static_cast<void>(::close(fd));

    if (!ok(result)) {
        return result;
    }
    if (!reached_end) {
        return (total == buffer.size()) ? Status::Capacity : Status::Io;
    }
    length = total;
    return Status::Ok;
}

Status read_cpu_list_file(const char* path, CpuSet& out) noexcept {
    PLANCK_EXPECT(path != nullptr);

    std::array<char, kMaxCpuListText> buffer{};
    std::size_t length = 0;
    if (const Status status = read_small_file(path, buffer, length); !ok(status)) {
        return status;
    }
    PLANCK_ASSERT(length < buffer.size());
    return parse_cpu_list(std::string_view{buffer.data(), length}, out);
}

}  // namespace

Status parse_cpu_list(std::string_view text, CpuSet& out) noexcept {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.size() > kMaxCpuListText) {
        return Status::OutOfRange;
    }

    CpuSet parsed;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t first = 0;
        if (const Status status = parse_cpu_number(text, pos, first); !ok(status)) {
            return status;
        }
        std::size_t last = first;
        if (pos < text.size() && text[pos] == '-') {
            ++pos;
            if (const Status status = parse_cpu_number(text, pos, last); !ok(status)) {
                return status;
            }
        }
        if (last < first) {
            return Status::Protocol;
        }
        parsed.set_range(first, last);

        if (pos < text.size()) {
            // A separator must be a comma and must be followed by another entry.
            if (text[pos] != ',' || (pos + 1) == text.size()) {
                return Status::Protocol;
            }
            ++pos;
        }
    }

    out = parsed;
    return Status::Ok;
}

Status isolated_cpus(CpuSet& out) noexcept {
    return read_cpu_list_file(kIsolatedPath, out);
}

Status nohz_full_cpus(CpuSet& out) noexcept {
    return read_cpu_list_file(kNohzFullPath, out);
}

std::size_t online_cpu_count() noexcept {
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    PLANCK_VERIFY(count >= 1);
    return static_cast<std::size_t>(count);
}

std::size_t current_cpu() noexcept {
    const int cpu = ::sched_getcpu();
    PLANCK_VERIFY(cpu >= 0);
    return static_cast<std::size_t>(cpu);
}

Status pin_current_thread(std::size_t cpu) noexcept {
    if (cpu >= kMaxCpus) {
        return Status::OutOfRange;
    }

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    // pid 0 means the calling thread, not the whole process.
    if (::sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        // EINVAL here means the cpu is offline or outside our cgroup's cpuset.
        return (errno == EINVAL) ? Status::OutOfRange : status_from_errno(errno);
    }
    return Status::Ok;
}

Status name_current_thread(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxThreadName) {
        return Status::InvalidArgument;
    }

    // string_view is not guaranteed to be terminated, so copy it.
    std::array<char, kMaxThreadName + 1> buffer{};
    std::memcpy(buffer.data(), name.data(), name.size());
    PLANCK_ASSERT(buffer[name.size()] == '\0');

    const int error = ::pthread_setname_np(::pthread_self(), buffer.data());
    return (error == 0) ? Status::Ok : status_from_errno(error);
}

Status set_realtime_priority(int priority) noexcept {
    if (priority < kMinRealtimePriority || priority > kMaxRealtimePriority) {
        return Status::InvalidArgument;
    }

    sched_param param{};
    param.sched_priority = priority;
    // pthread_* returns the error code directly and does not set errno.
    const int error = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
    return (error == 0) ? Status::Ok : status_from_errno(error);
}

Status configure_hot_thread(const HotThreadConfig& config, HotThreadReport& report) noexcept {
    report = HotThreadReport{};

    if (const Status status = pin_current_thread(config.cpu); !ok(status)) {
        return status;
    }
    report.pinned = true;

    if (const Status status = name_current_thread(config.name); !ok(status)) {
        return status;
    }

    // A kernel that does not expose these files has no isolated or tickless
    // CPUs, so a failed read means "no".
    CpuSet cpus;
    report.isolated = ok(isolated_cpus(cpus)) && cpus.test(config.cpu);
    report.tickless = ok(nohz_full_cpus(cpus)) && cpus.test(config.cpu);
    if (config.require_isolated && !report.isolated) {
        return Status::Unsupported;
    }

    if (config.realtime_priority != 0) {
        if (const Status status = set_realtime_priority(config.realtime_priority); !ok(status)) {
            return status;
        }
        report.realtime = true;
    }
    return Status::Ok;
}

}  // namespace planck::sys
