// src/core/document_files.hpp (internal to navigator_core)
//
// Purpose: the file rules shared by every JSON document Navigator keeps under its config root
//          (session.json, layouts/*.json, bookmarks.json, recents.json; navigator/layout.hpp
//          "Files", navigator/places.hpp "Files"): bounded reads, atomic private writes and the
//          ".bad" move-aside of a document that is not a JSON object. POSIX I/O only.
// Thread-safety: reentrant free functions, blocking file I/O; no shared state.
#pragma once

#include <string>
#include <string_view>

#include <navigator/types.hpp>

namespace navigator::core {

// "<what> '<path>': <strerror> (errno N)".
[[nodiscard]] std::string errno_message(std::string_view what, const Path& path, int err);

// errno of a failed write-side call -> PermissionDenied / OutOfMemory / IoError.
[[nodiscard]] ErrorCode map_write_errno(int err) noexcept;

// Reads a whole regular file of at most layout::kMaxDocumentBytes.
// Errors: InvalidArgument (empty path, too large), NotFound, PermissionDenied, OutOfMemory,
// IoError (not a regular file, read failure).
[[nodiscard]] Result<std::string> read_document(const Path& file);

// Unique temporary file in the target directory (mode 0600) -> write -> fsync -> rename over
// `file` -> fsync the directory. Missing parent directories are created with mode 0700. On
// failure the old file is untouched and no temporary file is left.
// Errors: InvalidArgument (empty path, no file name), PermissionDenied, IoError, OutOfMemory.
[[nodiscard]] Result<void> write_atomically(const Path& file, std::string_view text);

// Renames `file` to "<file>.bad" (replacing an older one) and returns InvalidArgument whose
// message is `reason` plus what happened to the file.
[[nodiscard]] Error move_aside_bad(const Path& file, std::string reason);

} // namespace navigator::core
