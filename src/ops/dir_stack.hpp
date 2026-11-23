// src/ops/dir_stack.hpp: a stack of open directory descriptors for dirfd-relative tree walks
// (S4, S5). Level 0 is the walk's root (always open); each push() opens a child with
// O_DIRECTORY | O_NOFOLLOW and checks its (st_dev, st_ino) against the identity recorded by the
// scan, so a directory swapped for a symlink or another directory mid-walk is never entered.
//
// Deep trees: at most kMaxOpen descriptors stay open. The shallowest intermediate levels are
// closed first; pop() reopens a closed parent through ".." of the current level (never a
// symlink) and verifies its identity again, so a tree of any depth is walked with a bounded
// number of descriptors and without path lookups.
//
// Thread-safety: one walk, one thread.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <navigator/types.hpp>

#include "sys.hpp"

namespace navigator::ops::detail {

class DirStack {
public:
    static constexpr std::size_t kMaxOpen = 48;

    DirStack(Fd root, FileId root_id) noexcept;

    [[nodiscard]] int fd() const noexcept { return levels_.back().fd.get(); }
    [[nodiscard]] FileId id() const noexcept { return levels_.back().id; }
    // Number of levels above the root (0: at the root).
    [[nodiscard]] std::size_t depth() const noexcept { return levels_.size() - 1; }

    // Enters child `name` of the current level. `expected` with device == 0 && inode == 0
    // accepts any directory. 0, or errno (ESTALE: identity differs from `expected`).
    [[nodiscard]] int push(const std::string& name, const FileId& expected);
    // Leaves the current level (no-op at the root). 0, or errno (ESTALE: the parent reached
    // through ".." is not the recorded one).
    [[nodiscard]] int pop();

private:
    struct Level {
        Fd fd;
        FileId id;
    };
    void enforce_limit() noexcept;

    std::vector<Level> levels_;
    std::size_t open_ = 1;
};

} // namespace navigator::ops::detail
