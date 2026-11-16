#include "errno_util.hpp"

#include <cerrno>
#include <system_error>

namespace navigator::platform_internal {

std::string errno_text(int err) { return std::error_code(err, std::generic_category()).message(); }

ErrorCode errno_code(int err, ErrorCode fallback) noexcept {
    switch (err) {
    case ENOENT: return ErrorCode::NotFound;
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case ENOMEM: return ErrorCode::OutOfMemory;
    case ENOTDIR:
    case EINVAL:
    case ELOOP:
    case ENAMETOOLONG: return ErrorCode::InvalidArgument;
    default: return fallback;
    }
}

Error errno_error(int err, std::string_view what, const Path& path, ErrorCode fallback) {
    std::string message(what);
    if (!path.empty()) {
        message += " '";
        message += path.native();
        message += '\'';
    }
    message += ": ";
    message += errno_text(err);
    return Error{errno_code(err, fallback), std::move(message)};
}

} // namespace navigator::platform_internal
