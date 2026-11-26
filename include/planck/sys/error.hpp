#ifndef PLANCK_SYS_ERROR_HPP
#define PLANCK_SYS_ERROR_HPP

#include "planck/core/status.hpp"

namespace planck::sys {

// Map an errno value to a Status. Never returns Ok: call this only after a
// system call has reported failure. Unrecognised values map to Io.
[[nodiscard]] core::Status status_from_errno(int error) noexcept;

}  // namespace planck::sys

#endif  // PLANCK_SYS_ERROR_HPP
