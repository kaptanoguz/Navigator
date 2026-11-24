// src/ops/tree.cpp: see tree.hpp.
#include "tree.hpp"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include "sys.hpp"

namespace navigator::ops::detail {
namespace {

[[nodiscard]] std::uint64_t space_estimate(const struct stat& st) noexcept {
    const auto size = static_cast<std::uint64_t>(st.st_size);
    const auto blocks = static_cast<std::uint64_t>(st.st_blocks) * 512U;
    // Sparse files keep their holes (S11); a filesystem reporting no blocks counts the size.
    return blocks > 0 && blocks < size ? blocks : size;
}

struct Frame {
    std::uint32_t node = 0;
    std::vector<std::string> children;
    std::size_t next = 0;
};

} // namespace

Path node_path(const Tree& tree, std::uint32_t i) {
    return tree.parent /
           node_rel(tree, i, [](const Node& n) -> const std::string& { return n.name; });
}

TreeCursor::TreeCursor(Fd root, FileId root_id, Tree& tree, Side side) noexcept
    : stack_(std::move(root), root_id), tree_(tree), side_(side) {}

Result<Tree> scan_tree(Context& ctx, const Path& top_in, bool follow_top) {
    Tree tree;
    tree.follow_top = follow_top;
    Path top = top_in;
    if (follow_top) {
        struct stat lst {};
        if (lstat_path(top, lst) == 0 && S_ISLNK(lst.st_mode)) {
            std::unique_ptr<char, decltype(&std::free)> real(::realpath(top.c_str(), nullptr),
                                                             &std::free);
            if (!real) {
                return Unexpected<Error>(errno_error(errno, "cannot resolve link", top));
            }
            top = Path(real.get());
            if (top == "/") {
                return fail(ErrorCode::InvalidArgument, "link target is the root folder");
            }
        }
    }
    tree.parent = top.parent_path();
    Fd parent(open_dir_path(tree.parent));
    if (!parent.valid()) {
        return Unexpected<Error>(errno_error(errno, "cannot open folder", tree.parent));
    }
    struct stat pst {};
    if (::fstat(parent.get(), &pst) != 0) {
        return Unexpected<Error>(errno_error(errno, "cannot stat", tree.parent));
    }
    tree.parent_id = id_of(pst);

    Node root;
    root.name = top.filename().native();
    const int rc = lstat_at(parent.get(), root.name, root.st);
    if (rc != 0) {
        return Unexpected<Error>(errno_error(rc, "cannot stat", top));
    }
    const dev_t top_dev = root.st.st_dev;
    tree.nodes.push_back(std::move(root));
    auto account = [&](const struct stat& st) {
        std::uint64_t bytes = 0;
        if (S_ISREG(st.st_mode)) {
            bytes = static_cast<std::uint64_t>(st.st_size);
            tree.bytes += bytes;
            tree.space += space_estimate(st);
        }
        ctx.add_total(1, bytes);
    };
    account(tree.nodes[0].st);
    if (!S_ISDIR(tree.nodes[0].st.st_mode)) {
        return tree;
    }

    DirStack stack(std::move(parent), tree.parent_id);
    std::vector<Frame> frames;
    auto enter = [&](std::uint32_t idx) -> bool {
        Node& n = tree.nodes[idx];
        const int prc = stack.push(n.name, id_of(n.st));
        if (prc != 0) {
            n.scan_errno = prc;
            return false;
        }
        Frame f;
        f.node = idx;
        const int rrc = read_dir_names(stack.fd(), f.children);
        if (rrc != 0) {
            n.scan_errno = rrc;
            (void)stack.pop();
            return false;
        }
        frames.push_back(std::move(f));
        return true;
    };
    (void)enter(0);
    std::size_t tick = 0;
    while (!frames.empty()) {
        if ((++tick & 63U) == 0 && !ctx.checkpoint()) {
            return fail(ErrorCode::Cancelled, "cancelled while scanning");
        }
        Frame& f = frames.back();
        if (f.next >= f.children.size()) {
            frames.pop_back();
            const int prc = stack.pop();
            if (prc != 0) {
                return Unexpected<Error>(errno_error(prc, "folder changed while scanning", top));
            }
            continue;
        }
        Node child;
        child.name = std::move(f.children[f.next++]);
        child.parent = f.node;
        const int src = lstat_at(stack.fd(), child.name, child.st);
        if (src == ENOENT) {
            continue; // vanished since readdir: not part of the operation
        }
        if (src != 0) {
            child.scan_errno = src;
        }
        const bool is_dir = src == 0 && S_ISDIR(child.st.st_mode);
        if (is_dir && child.st.st_dev != top_dev) {
            child.mount_point = true; // S5: reported, never entered
        }
        account(child.st);
        tree.nodes.push_back(std::move(child));
        const auto idx = static_cast<std::uint32_t>(tree.nodes.size() - 1);
        if (is_dir && !tree.nodes[idx].mount_point) {
            (void)enter(idx); // `f` may dangle after this; it is not used again
        }
    }
    return tree;
}

} // namespace navigator::ops::detail
