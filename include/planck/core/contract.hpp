#ifndef PLANCK_CORE_CONTRACT_HPP
#define PLANCK_CORE_CONTRACT_HPP

// Contract checks (NASA Power of Ten, rule 5).
//
//   PLANCK_EXPECT(cond)  precondition
//   PLANCK_ENSURE(cond)  postcondition
//   PLANCK_ASSERT(cond)  invariant inside a function
//   PLANCK_VERIFY(cond)  always checked, in every mode. Cold paths only.
//
// PLANCK_CONTRACT_MODE selects what the first three do:
//   0  off      compiled out
//   1  assume   handed to the optimizer as a fact, no code emitted
//   2  checked  tested at runtime, aborts with file and line on failure
//
// Default is checked without NDEBUG and assume with it. Conditions must be
// free of side effects, since modes 0 and 1 never evaluate them.

#define PLANCK_CONTRACT_OFF 0
#define PLANCK_CONTRACT_ASSUME 1
#define PLANCK_CONTRACT_CHECKED 2

#if !defined(PLANCK_CONTRACT_MODE)
#if defined(NDEBUG)
#define PLANCK_CONTRACT_MODE PLANCK_CONTRACT_ASSUME
#else
#define PLANCK_CONTRACT_MODE PLANCK_CONTRACT_CHECKED
#endif
#endif

#if PLANCK_CONTRACT_MODE < PLANCK_CONTRACT_OFF || PLANCK_CONTRACT_MODE > PLANCK_CONTRACT_CHECKED
#error "PLANCK_CONTRACT_MODE must be 0 (off), 1 (assume) or 2 (checked)"
#endif

namespace planck::core {

// Prints the failed contract to stderr and aborts. Never returns.
[[noreturn]] void contract_failure(const char* kind, const char* expr, const char* file,
                                   int line) noexcept;

}  // namespace planck::core

#define PLANCK_CONTRACT_CHECK_(kind, cond)                                          \
    do {                                                                            \
        if (!(cond)) [[unlikely]] {                                                 \
            ::planck::core::contract_failure(kind, #cond, __FILE__, __LINE__);      \
        }                                                                           \
    } while (false)

#if PLANCK_CONTRACT_MODE == PLANCK_CONTRACT_CHECKED
#define PLANCK_CONTRACT_(kind, cond) PLANCK_CONTRACT_CHECK_(kind, cond)
#elif PLANCK_CONTRACT_MODE == PLANCK_CONTRACT_ASSUME
#if defined(__has_cpp_attribute) && __has_cpp_attribute(assume) >= 202207L
#define PLANCK_CONTRACT_(kind, cond) [[assume(cond)]]
#else
#error "contract mode 'assume' needs [[assume]]: GCC 13+ or Clang 19+"
#endif
#else
#define PLANCK_CONTRACT_(kind, cond) static_cast<void>(0)
#endif

#define PLANCK_EXPECT(cond) PLANCK_CONTRACT_("precondition", cond)
#define PLANCK_ENSURE(cond) PLANCK_CONTRACT_("postcondition", cond)
#define PLANCK_ASSERT(cond) PLANCK_CONTRACT_("invariant", cond)
#define PLANCK_VERIFY(cond) PLANCK_CONTRACT_CHECK_("verify", cond)

#endif  // PLANCK_CORE_CONTRACT_HPP
