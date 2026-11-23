// src/ops/move.hpp: same-filesystem moves and renames (S7: renameat2 RENAME_NOREPLACE; plain
// renameat only after an explicit Replace decision), merges of a folder onto a folder, and the
// cross-filesystem fallback through tree.hpp / copy.hpp. Also the execution-time source checks
// (S13, S15) shared by every kind that moves, trashes or deletes user items.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <navigator/ops.hpp>

#include "context.hpp"
#include "tree.hpp"

namespace navigator::ops::detail {

struct MoveParams {
    OpKind step_kind = OpKind::Move;    // kind of the produced UndoSteps (Move or Rename)
    std::optional<FileId> auto_replace; // Redo: replace an existing item with this identity
    bool permanent_replace_ok = true;   // false for Undo / Redo
    bool allow_cross_device = true;     // Rename: false (same folder)
};

struct MoveResult {
    ItemOutcome outcome;
    std::vector<UndoStep> steps;
};

// Moves `src` (absolute, never followed) to dst_dir / dst_name: rename within one filesystem,
// copy + verify + remove across filesystems. `prescanned`: the tree from the Scanning phase
// (cross-filesystem items found up front). No runtime source checks here (see below).
[[nodiscard]] MoveResult move_item(Context& ctx, const Path& src, const Path& dst_dir,
                                   const std::string& dst_name, const MoveParams& params,
                                   Tree* prescanned);

// Execution-time checks of a source that is moved, renamed, trashed or deleted (S13, S15):
// it exists (lstat), is not a mount point (st_dev differs from its folder's), and its canonical
// location (realpath of the folder + name) is not protected. With `into` (Move / Copy): a
// folder is not placed into itself or below itself (canonical, so symlinked destinations are
// caught too). `protect`: apply the protected-path rule (false for Copy).
[[nodiscard]] std::optional<Error> check_source(Context& ctx, const Path& src, bool protect,
                                                const Path* into);

} // namespace navigator::ops::detail
