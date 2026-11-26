#include "planck/core/contract.hpp"

#include <cstdio>
#include <cstdlib>

namespace planck::core {

[[gnu::cold, gnu::noinline]] void contract_failure(const char* kind, const char* expr,
                                                   const char* file, int line) noexcept {
    const char* const safe_kind = (kind != nullptr) ? kind : "contract";
    const char* const safe_expr = (expr != nullptr) ? expr : "?";
    const char* const safe_file = (file != nullptr) ? file : "?";

    // Return values are ignored on purpose. The process is about to abort and
    // there is nothing useful to do if stderr is gone.
    static_cast<void>(std::fprintf(stderr, "planck: %s failed: %s\n  at %s:%d\n", safe_kind,
                                   safe_expr, safe_file, line));
    static_cast<void>(std::fflush(stderr));
    std::abort();
}

}  // namespace planck::core
