// src/ops/tree.hpp: the scan of one top-level item (S4, S5, S14): a pre-order list of every
// entry with its lstat, walked dirfd-relative without following links and without entering a
// directory on another filesystem. Copies, cross-filesystem moves and permanent deletes act
// only on entries recorded here ("only entries recorded in the scan are removed", S7).
#pragma once

#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <navigator/ops.hpp>

#include "context.hpp"
#include "dir_stack.hpp"

namespace navigator::ops::detail {

inline constexpr std::uint32_t kNoParent = 0xFFFFFFFFU;

struct Node {
    std::string name; // entry name (node 0: the top-level item's name in its parent)
    std::uint32_t parent = kNoParent;
    struct stat st {};
    int scan_errno = 0;       // a directory that could not be read (its children are unknown)
    bool mount_point = false; // a directory on another filesystem (S5): never entered

    // Execution state (copy / move / delete).
    enum class State : std::uint8_t { Pending, Created, Merged, Skipped, Failed, Removed };
    State state = State::Pending;
    bool verified = false;   // cross-fs move: the source file was unchanged during its copy
    bool kept_child = false; // some descendant stays (not removed)
    std::string dst_name;
    FileId dst_id;
    std::optional<trash::TrashedItem> replaced; // S8
    bool replaced_permanently = false;
};

struct Tree {
    Path parent;      // the folder holding the top-level item
    FileId parent_id; // its identity when scanned
    bool follow_top = false;
    std::vector<Node> nodes; // pre-order: every directory precedes its descendants
    std::uint64_t bytes = 0; // logical size of the regular files (progress)
    std::uint64_t space = 0; // estimated allocation (S14)
};

// Scans `top` (absolute). follow_top: stat the top-level item through a symlink (Copy with
// dereference_top_level). Totals go to ctx as they grow. Errors: the top-level item itself
// cannot be reached (NotFound, PermissionDenied, ...), Cancelled.
[[nodiscard]] Result<Tree> scan_tree(Context& ctx, const Path& top, bool follow_top);

// Absolute source path of node `i`.
[[nodiscard]] Path node_path(const Tree& tree, std::uint32_t i);
// Relative path of node `i` below the top-level item's parent, using names chosen by `pick`.
template <class Pick> [[nodiscard]] Path node_rel(const Tree& tree, std::uint32_t i, Pick pick) {
    std::vector<std::uint32_t> chain;
    for (std::uint32_t k = i; k != kNoParent; k = tree.nodes[k].parent) {
        chain.push_back(k);
    }
    Path out;
    for (const std::uint32_t k : std::views::reverse(chain)) {
        out /= pick(tree.nodes[k]);
    }
    return out;
}

// A DirStack that follows a Tree: level k (k >= 1) holds node chain[k-1]. go_to(p) makes node
// `p` the current directory (kNoParent: the root), popping and pushing as needed. `on_pop` is
// called with (node index, fd) before a level is left (directory metadata, fsync).
class TreeCursor {
public:
    enum class Side : std::uint8_t { Source, Destination };
    TreeCursor(Fd root, FileId root_id, Tree& tree, Side side) noexcept;

    template <class OnPop> [[nodiscard]] int go_to(std::uint32_t target, OnPop&& on_pop);
    [[nodiscard]] int go_to(std::uint32_t target) {
        return go_to(target, [](std::uint32_t, int) {});
    }
    template <class OnPop> [[nodiscard]] int unwind(OnPop&& on_pop) {
        return go_to(kNoParent, on_pop);
    }
    [[nodiscard]] int fd() const noexcept { return stack_.fd(); }

private:
    DirStack stack_;
    Tree& tree_;
    Side side_;
    std::vector<std::uint32_t> nodes_;
    std::vector<std::uint32_t> chain_;
};

template <class OnPop> int TreeCursor::go_to(std::uint32_t target, OnPop&& on_pop) {
    chain_.clear();
    for (std::uint32_t k = target; k != kNoParent; k = tree_.nodes[k].parent) {
        chain_.push_back(k);
    }
    // chain_ is target..top; compare with nodes_ (top..current) from the top.
    std::size_t common = 0;
    while (common < nodes_.size() && common < chain_.size() &&
           nodes_[common] == chain_[chain_.size() - 1 - common]) {
        ++common;
    }
    while (nodes_.size() > common) {
        on_pop(nodes_.back(), stack_.fd());
        const int rc = stack_.pop();
        nodes_.pop_back();
        if (rc != 0) {
            return rc;
        }
    }
    for (std::size_t k = common; k < chain_.size(); ++k) {
        const std::uint32_t n = chain_[chain_.size() - 1 - k];
        const Node& node = tree_.nodes[n];
        const bool src = side_ == Side::Source;
        const int rc =
            stack_.push(src ? node.name : node.dst_name, src ? id_of(node.st) : node.dst_id);
        if (rc != 0) {
            return rc;
        }
        nodes_.push_back(n);
    }
    return 0;
}

} // namespace navigator::ops::detail
