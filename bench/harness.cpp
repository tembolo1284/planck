#include "harness.hpp"

#include <cstdio>

#include "planck/core/contract.hpp"
#include "planck/sys/cpu.hpp"

namespace planck::bench {

core::Status Stopwatch::calibrate() noexcept {
    if (const core::Status status = clock_.calibrate(); !core::ok(status)) {
        return status;
    }
    nanos_per_tick_ = 1e9 / static_cast<double>(clock_.ticks_per_second());

    overhead_.reset();
    for (std::uint64_t i = 0; i < kOverheadSamples; ++i) {
        const std::uint64_t started = sys::tsc_now();
        overhead_.record(sys::tsc_now_serialized() - started);
    }
    overhead_ticks_ = overhead_.min();
    return core::Status::Ok;
}

void Table::header() const noexcept {
    if (csv_) {
        std::printf("benchmark,samples,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,mean_ns\n");
        return;
    }
    std::printf("%-26s %10s %9s %9s %9s %9s %9s %9s %11s\n", "benchmark (ns)", "samples", "min",
                "p50", "p90", "p99", "p99.9", "p99.99", "max");
}

void Table::row(std::string_view name, const perf::Histogram& ticks) const noexcept {
    const perf::Summary s = ticks.summary();
    const auto ns = [this](std::uint64_t value) { return watch_->to_nanos(value); };
    const int width = static_cast<int>(name.size());
    if (csv_) {
        std::printf("%.*s,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", width, name.data(),
                    static_cast<unsigned long long>(s.count), ns(s.min), ns(s.p50), ns(s.p90),
                    ns(s.p99), ns(s.p999), ns(s.p9999), ns(s.max),
                    s.mean * watch_->to_nanos(1));
        return;
    }
    std::printf("%-26.*s %10llu %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %11.1f\n", width, name.data(),
                static_cast<unsigned long long>(s.count), ns(s.min), ns(s.p50), ns(s.p90),
                ns(s.p99), ns(s.p999), ns(s.p9999), ns(s.max));
}

void print_environment(const Stopwatch& watch) noexcept {
#if defined(__OPTIMIZE__)
    const char* const optimised = "optimised";
#else
    const char* const optimised = "NOT OPTIMISED";
#endif
#if PLANCK_CONTRACT_MODE == PLANCK_CONTRACT_CHECKED
    const char* const contracts = "checked";
#elif PLANCK_CONTRACT_MODE == PLANCK_CONTRACT_ASSUME
    const char* const contracts = "assumed";
#else
    const char* const contracts = "off";
#endif
    std::fprintf(stderr,
                 "# %zu cpus, on cpu %zu, counter %.3f GHz%s, timer cost %.1f ns (taken off), "
                 "build %s, contracts %s\n",
                 sys::online_cpu_count(), sys::current_cpu(),
                 static_cast<double>(watch.ticks_per_second()) / 1e9,
                 sys::tsc_is_invariant() ? "" : " (NOT INVARIANT)",
                 watch.to_nanos(watch.overhead_ticks()), optimised, contracts);
#if !defined(__OPTIMIZE__) || PLANCK_CONTRACT_MODE == PLANCK_CONTRACT_CHECKED
    std::fprintf(stderr, "# these numbers are not the engine's speed: use a Release build\n");
#endif
}

}  // namespace planck::bench
