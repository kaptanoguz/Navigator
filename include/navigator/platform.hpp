// navigator/platform.hpp
//
// Purpose: desktop integration: open with default app, trash, MIME lookup, terminal, XDG dirs.
//          Backends (GIO, portals, xdg-open, ...) stay inside src/platform; none leak here.
// Owner:   contracts (header); implementation in src/platform (role: platform). STUB until
//          Phase 1+.
// Thread-safety: functions are reentrant and may block (D-Bus, process spawn); call them off the
//                UI thread or through the ops/job system.
#pragma once

#include <string>

#include <navigator/types.hpp>

namespace navigator::platform {

// Phase 1+. Opens `path` with the user's default application.
// Errors: NotFound, Unsupported (no handler), PlatformError.
[[nodiscard]] Result<void> open_with_default(const Path& path);

// Phase 1+. Moves `path` to the freedesktop.org trash of its volume.
// Errors: NotFound, PermissionDenied, Unsupported (no trash on this volume), IoError.
[[nodiscard]] Result<void> move_to_trash(const Path& path);

// Phase 1+. MIME type such as "image/png"; "application/octet-stream" when unknown.
[[nodiscard]] Result<std::string> mime_type(const Path& path);

// Phase 1+. Opens the user's terminal emulator with `dir` as working directory.
[[nodiscard]] Result<void> open_terminal(const Path& dir);

enum class UserDir { Home, Desktop, Documents, Downloads, Music, Pictures, Videos, Config, Cache };

// Phase 1+. XDG user/base directory.
[[nodiscard]] Result<Path> user_dir(UserDir which);

} // namespace navigator::platform
