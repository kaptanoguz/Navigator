// src/core/entry_meta.hpp (internal to navigator_core)
//
// Purpose: the per-entry metadata rules of fs::list_directory() (navigator/fs.hpp), shared with
//          fs::Walker (walk.cpp) so a walked entry is described exactly like a listed one.
// Thread-safety: free functions are reentrant; a Credentials value is immutable after
//                construction and may be shared read-only.
#pragma once

#include <cstdint>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include <navigator/fs.hpp>

namespace navigator::core {

// Errors of stat() and of per-entry metadata calls (ENOENT/ENOTDIR -> NotFound, ...).
[[nodiscard]] ErrorCode map_stat_errno(int err) noexcept;

[[nodiscard]] fs::EntryKind kind_from_mode(mode_t mode) noexcept;
[[nodiscard]] fs::EntryKind kind_from_dtype(unsigned char type) noexcept;

// Effective credentials, captured once (per listing or per walk).
class Credentials {
public:
    Credentials();

    [[nodiscard]] bool can_read(std::uint32_t mode, std::uint32_t uid,
                                std::uint32_t gid) const noexcept;

private:
    [[nodiscard]] bool in_group(std::uint32_t gid) const noexcept;

    uid_t uid_;
    gid_t gid_;
    std::vector<gid_t> groups_;
};

// Fills every field except name/hidden from `lst`, the lstat result of `name` relative to
// `dirfd`; for a symlink one more fstatat of the target fills target_kind / dangling (and the
// metadata when follow_symlinks).
void describe_entry(fs::DirEntry& entry, int dirfd, const char* name, const struct stat& lst,
                    bool follow_symlinks, const Credentials& creds) noexcept;

} // namespace navigator::core
