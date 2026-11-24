// Internal (src/platform): building blocks of <navigator/trash.hpp> shared by trash_format.cpp
// and trash.cpp, exposed to tests/platform. Not part of the public contract.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <navigator/trash.hpp>

namespace navigator::trash_internal {

using trash::TrashDir;
using trash::TrashDirKind;

// Lexically normal absolute form without a trailing '/' (except "/" itself).
[[nodiscard]] Path normal_path(const Path& p);

// True when `path` equals `base` or lies below it, component-wise. Both lexically normal.
[[nodiscard]] bool at_or_below(const Path& path, const Path& base);

// Longest prefix of `s` of at most `max_bytes` bytes that does not end inside a UTF-8 multi-byte
// sequence (invalid UTF-8 is cut byte-wise).
[[nodiscard]] std::string_view cut_utf8(std::string_view s, std::size_t max_bytes) noexcept;

// Candidate number `n` (1-based) for an item called `base` (T5): n == 1 -> base, else
// base + "." + n; `base` is cut (cut_utf8) so that candidate + ".trashinfo" fits NAME_MAX.
[[nodiscard]] std::string candidate_name(std::string_view base, unsigned n);

// T4 for one volume: the per-volume trash on `topdir` for `uid`.
//   create == false: never changes anything; a directory that trash() would create counts when
//                    its parent is writable (trash_dir_for()).
//   create == true:  creates $topdir/.Trash/$uid or $topdir/.Trash-$uid (0700) when missing.
// Errors: Unsupported (no usable trash on this volume), ReadOnlyFilesystem, PermissionDenied
// (an existing trash directory that is not writable), IoError.
[[nodiscard]] Result<TrashDir> select_topdir_trash(const Path& topdir, std::uint32_t uid,
                                                   bool create);

// The existing, valid trashes on `topdir` (SharedUser first, then User); never creates.
[[nodiscard]] std::vector<TrashDir> existing_topdir_trashes(const Path& topdir, std::uint32_t uid);

} // namespace navigator::trash_internal
