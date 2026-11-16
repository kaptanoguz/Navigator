// Internal (src/platform): errno -> navigator::Error mapping shared by the platform backends.
#pragma once

#include <string>
#include <string_view>

#include <navigator/types.hpp>

namespace navigator::platform_internal {

// Human-readable errno text ("No such file or directory"), English, thread-safe.
[[nodiscard]] std::string errno_text(int err);

// ENOENT -> NotFound, EACCES/EPERM -> PermissionDenied, ENOMEM -> OutOfMemory,
// ENOTDIR/EINVAL/ELOOP/ENAMETOOLONG -> InvalidArgument, anything else -> `fallback`.
[[nodiscard]] ErrorCode errno_code(int err, ErrorCode fallback = ErrorCode::IoError) noexcept;

// Error{errno_code(err, fallback), "<what> '<path>': <errno text>"} (path omitted when empty).
[[nodiscard]] Error errno_error(int err, std::string_view what, const Path& path,
                                ErrorCode fallback = ErrorCode::IoError);

} // namespace navigator::platform_internal
