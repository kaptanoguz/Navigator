// src/ops/conflict.hpp: name-conflict resolution shared by every kind that places an item
// (S8): policy mapping, the Ask handshake with validation of the answer, KeepBoth names, and
// the "trash the existing item first" step of Replace.
#pragma once

#include <string>
#include <string_view>

#include <sys/stat.h>

#include <navigator/ops.hpp>

#include "context.hpp"

namespace navigator::ops::detail {

struct ConflictInput {
    Path source_path;
    const struct stat* source = nullptr; // nullptr: the new item is an empty folder to create
    int dst_dirfd = -1;
    Path dst_dir;
    std::string name; // the taken name in dst_dir
    struct stat existing {};
    bool force_keep_both = false;     // Copy into the sources' own folder
    bool merge_supported = true;      // the caller can merge directory onto directory
    bool replace_supported = true;    // the caller can replace at all
    bool permanent_replace_ok = true; // false for Undo / Redo (S12: never overwrite)
};

struct Placement {
    enum class Action : std::uint8_t { Skip, Replace, KeepBoth, Merge, Cancel, Fail };
    Action action = Action::Skip;
    std::string name;       // KeepBoth: the free name; otherwise the taken name
    bool permanent = false; // Replace without a trash copy (S8, the record becomes lossy)
    struct stat existing {};
    Error error;
};

// Applies OpRequest::conflict (or asks) for one taken name. Never touches the filesystem except
// to look for free KeepBoth names and to ask ITrash::trash_dir_for.
[[nodiscard]] Placement resolve_placement(Context& ctx, const ConflictInput& in);

// First keep_both_name(name, n, is_dir), n = 2.., that does not exist in `dirfd`. 0 or errno.
[[nodiscard]] int first_free_name(int dirfd, std::string_view name, bool is_dir, std::string& out);

// Moves the existing entry `name` of `dirfd` (== `dir`) to the trash after checking that it is
// still the inode the decision was made about. Errors: Unsupported (no trash), AlreadyExists
// (the entry changed since), ITrash errors.
[[nodiscard]] Result<trash::TrashedItem> trash_existing(Context& ctx, int dirfd, const Path& dir,
                                                        const std::string& name,
                                                        const struct stat& existing);

// True when ITrash would take `path` (trash present, OpOptions::trash_replaced).
[[nodiscard]] bool replace_can_trash(const Context& ctx, const Path& path);

} // namespace navigator::ops::detail
