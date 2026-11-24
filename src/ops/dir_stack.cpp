// src/ops/dir_stack.cpp: see dir_stack.hpp.
#include "dir_stack.hpp"

#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace navigator::ops::detail {

DirStack::DirStack(Fd root, FileId root_id) noexcept {
    levels_.push_back(Level{std::move(root), root_id});
}

int DirStack::push(const std::string& name, const FileId& expected) {
    Fd child(open_dir_at(fd(), name));
    if (!child.valid()) {
        return errno;
    }
    struct stat st {};
    if (::fstat(child.get(), &st) != 0) {
        return errno;
    }
    const FileId actual = id_of(st);
    if ((expected.device != 0 || expected.inode != 0) && !(actual == expected)) {
        return ESTALE;
    }
    levels_.push_back(Level{std::move(child), actual});
    ++open_;
    enforce_limit();
    return 0;
}

int DirStack::pop() {
    if (levels_.size() <= 1) {
        return 0;
    }
    Level& parent = levels_[levels_.size() - 2];
    if (!parent.fd.valid()) {
        Fd up(::openat(fd(), "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!up.valid()) {
            return errno;
        }
        struct stat st {};
        if (::fstat(up.get(), &st) != 0) {
            return errno;
        }
        if (!(id_of(st) == parent.id)) {
            return ESTALE;
        }
        parent.fd = std::move(up);
        ++open_;
    }
    levels_.pop_back();
    --open_;
    return 0;
}

void DirStack::enforce_limit() noexcept {
    // Close the shallowest open intermediate level (never the root, never the current one).
    for (std::size_t i = 1; open_ > kMaxOpen && i + 1 < levels_.size(); ++i) {
        if (levels_[i].fd.valid()) {
            levels_[i].fd.reset();
            --open_;
        }
    }
}

} // namespace navigator::ops::detail
