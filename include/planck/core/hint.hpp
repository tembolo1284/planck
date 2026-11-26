#ifndef PLANCK_CORE_HINT_HPP
#define PLANCK_CORE_HINT_HPP

#if !defined(__GNUC__)
#error "planck requires GCC or Clang"
#endif

namespace planck::core {

// Pull a cache line in ahead of a read. Safe on any address, including null.
[[gnu::always_inline]] inline void prefetch_read(const void* addr) noexcept {
    __builtin_prefetch(addr, 0, 3);
}

// Pull a cache line in exclusive state ahead of a write.
[[gnu::always_inline]] inline void prefetch_write(const void* addr) noexcept {
    __builtin_prefetch(addr, 1, 3);
}

// One iteration of a spin-wait. Eases the pipeline flush on loop exit and
// yields the core to a hyperthread sibling.
[[gnu::always_inline]] inline void cpu_relax() noexcept {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
#error "planck supports x86-64 and AArch64 only"
#endif
}

}  // namespace planck::core

#endif  // PLANCK_CORE_HINT_HPP
