// src/ops/copy.hpp: placing a scanned tree at a destination (S6, S7, S9, S10, S11): Copy, the
// copy half of a cross-filesystem Move (with verification, source removal and rollback), and
// the cross-device Restore.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <navigator/ops.hpp>

#include "context.hpp"
#include "tree.hpp"

namespace navigator::ops::detail {

struct TransferParams {
    OpKind step_kind = OpKind::Copy;    // kind of the produced UndoSteps
    bool move = false;                  // cross-filesystem move: fsync, verify, remove sources
    bool force_keep_both = false;       // Copy into the sources' own folder
    Path dst_dir;                       // destination folder (absolute)
    std::string dst_name;               // name of the top-level item there
    std::optional<FileId> auto_replace; // Redo: replace an existing item with this identity
    bool permanent_replace_ok = true;
    bool inject_top = true; // call the fault injector for the top-level item too
};

struct TransferResult {
    ItemOutcome outcome;
    std::vector<UndoStep> steps;
    bool source_removed = false; // move: the top-level source entry is gone
};

[[nodiscard]] TransferResult transfer_tree(Context& ctx, Tree& tree, const TransferParams& params);

// Copies extended attributes ("user.*", POSIX ACLs; never security.* / trusted.*, S10). Returns
// the number of attributes that could not be copied.
[[nodiscard]] std::size_t copy_xattrs(int src_fd, int dst_fd, bool is_dir);

} // namespace navigator::ops::detail
