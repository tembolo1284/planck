#include "planck/sys/error.hpp"

#include <cerrno>

namespace planck::sys {

core::Status status_from_errno(int error) noexcept {
    using core::Status;

    // EAGAIN and EWOULDBLOCK share a value on Linux, so they cannot both be
    // case labels.
    if (error == EAGAIN || error == EWOULDBLOCK) {
        return Status::WouldBlock;
    }

    switch (error) {
        case EPERM:
        case EACCES:
            return Status::Permission;
        case EINVAL:
            return Status::InvalidArgument;
        case ENOMEM:
        case ENOSPC:
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
            return Status::Capacity;
        case ENOENT:
        case ESRCH:
            return Status::NotFound;
        case EEXIST:
        case EADDRINUSE:
            return Status::Duplicate;
        case EPIPE:
        case ECONNRESET:
        case ECONNABORTED:
        case ENOTCONN:
            return Status::Closed;
        case ERANGE:
        case EOVERFLOW:
            return Status::OutOfRange;
        case ENOSYS:
        case EOPNOTSUPP:
            return Status::Unsupported;
        default:
            return Status::Io;
    }
}

}  // namespace planck::sys
