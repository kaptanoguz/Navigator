// src/ops/kinds.hpp: one executor per OpKind (engine.cpp, simple_ops.cpp, undo_exec.cpp) and the
// single-item primitives that Undo / Redo reuse.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <navigator/ops.hpp>

#include "context.hpp"

namespace navigator::ops::detail {

// A Result error is a whole-request failure (S14 NoSpace); per-item outcomes go to ctx.
[[nodiscard]] Result<void> run_copy(Context& ctx);
[[nodiscard]] Result<void> run_move(Context& ctx);
[[nodiscard]] Result<void> run_rename(Context& ctx);
[[nodiscard]] Result<void> run_make_directory(Context& ctx);
[[nodiscard]] Result<void> run_create_symlink(Context& ctx);
[[nodiscard]] Result<void> run_trash(Context& ctx);
[[nodiscard]] Result<void> run_restore(Context& ctx);
[[nodiscard]] Result<void> run_delete(Context& ctx);
[[nodiscard]] Result<void> run_undo(Context& ctx);
[[nodiscard]] Result<void> run_redo(Context& ctx);

struct PlaceOne {
    ItemOutcome outcome;
    std::vector<UndoStep> steps;
};

// mkdir dir/name (0777 & ~umask); a taken name conflicts (Skip / KeepBoth only).
[[nodiscard]] PlaceOne make_directory_at(Context& ctx, const Path& dir, const std::string& name);
// symlink dir/name -> target; a taken name conflicts (Skip / KeepBoth / Replace).
[[nodiscard]] PlaceOne create_symlink_at(Context& ctx, const Path& target, const Path& dir,
                                         const std::string& name, bool permanent_replace_ok);
// ITrash::trash(src) with untrashable reporting (S2). No runtime source checks here.
[[nodiscard]] PlaceOne trash_one(Context& ctx, const Path& src);
// Puts `item` back at `dest` (missing parents created and recorded), conflicts per policy,
// cross-device restores copied then erased (S7). `auto_replace`: Redo.
[[nodiscard]] PlaceOne restore_one(Context& ctx, const trash::TrashedItem& item, const Path& dest,
                                   const std::optional<FileId>& auto_replace);
// Creates the missing folders of `dir`, outermost first. Errors: a component is not a folder.
[[nodiscard]] Result<std::vector<ItemGuard>> ensure_parents(const Path& dir);

} // namespace navigator::ops::detail
