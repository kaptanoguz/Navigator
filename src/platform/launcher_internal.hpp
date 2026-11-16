// Internal (src/platform): program lookup of the default IAppLauncher, exposed as a test seam.
#pragma once

#include <string>
#include <string_view>

#include <navigator/types.hpp>

namespace navigator::platform_internal {

// Resolves `name` like execvp(3): a name containing '/' is used as is; otherwise each entry of
// `path_env` (colon separated, an empty entry meaning the current directory) is tried in order.
// path_env == nullptr means "$PATH unset" and uses confstr(_CS_PATH) ("/bin:/usr/bin").
// A candidate qualifies when it is a regular file (after following symlinks) that the effective
// ids may execute (faccessat AT_EACCESS).
// Errors: InvalidArgument (empty name or a NUL byte in it), NotFound (no candidate qualifies).
[[nodiscard]] Result<std::string> resolve_program(std::string_view name, const char* path_env);

} // namespace navigator::platform_internal
