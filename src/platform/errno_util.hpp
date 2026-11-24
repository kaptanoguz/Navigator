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

// Phase 7 mapping for file operations (trash): errno_code() plus EROFS -> ReadOnlyFilesystem,
// ENOSPC/EDQUOT -> NoSpace, EXDEV -> CrossDevice, EEXIST -> AlreadyExists, ENOTEMPTY ->
// NotEmpty. Kept separate so the Phase 1-6 producers keep their documented codes.
[[nodiscard]] ErrorCode ops_errno_code(int err, ErrorCode fallback = ErrorCode::IoError) noexcept;

// errno_error() with ops_errno_code().
[[nodiscard]] Error ops_errno_error(int err, std::string_view what, const Path& path,
                                    ErrorCode fallback = ErrorCode::IoError);

} // namespace navigator::platform_internal
