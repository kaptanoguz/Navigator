// src/ops/move.cpp: see move.hpp.
#include "move.hpp"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>

#include "conflict.hpp"
#include "copy.hpp"
#include "sys.hpp"

namespace navigator::ops::detail {
namespace {

constexpr int kMaxAttempts = 6;

[[nodiscard]] MoveResult failed(const Path& src, Error e) {
    MoveResult r;
    r.outcome.source = src;
    r.outcome.status = ItemStatus::Failed;
    r.outcome.error = std::move(e);
    return r;
}

[[nodiscard]] MoveResult with_status(const Path& src, ItemStatus status) {
    MoveResult r;
    r.outcome.source = src;
    r.outcome.status = status;
    if (status == ItemStatus::Cancelled) {
        r.outcome.error = make_error(ErrorCode::Cancelled, "cancelled");
    }
    return r;
}

struct RenameResult {
    MoveResult result;
    bool cross_device = false;
};

RenameResult rename_item(Context& ctx, const Path& src, const Path& dst_dir, std::string dst_name,
                         const MoveParams& params, int depth);

// Folder onto folder (ConflictAction::Merge): every child is moved into the existing folder and
// may conflict again. The emptied source folder itself stays (S1: nothing in the contract allows
// removing it on a same-filesystem move).
MoveResult merge_into(Context& ctx, const Path& src, const Path& existing, const MoveParams& params,
                      int depth) {
    MoveResult out;
    out.outcome.source = src;
    out.outcome.destination = existing;
    out.outcome.status = ItemStatus::Done;
    Fd dir(open_dir_path(src));
    std::vector<std::string> names;
    if (!dir.valid() || read_dir_names(dir.get(), names) != 0) {
        return failed(src, errno_error(errno, "cannot read folder", src));
    }
    dir.reset();
    for (const std::string& name : names) {
        if (!ctx.checkpoint()) {
            out.outcome.status = ItemStatus::Cancelled;
            out.outcome.partial = true;
            out.outcome.error = make_error(ErrorCode::Cancelled, "cancelled");
            break;
        }
        RenameResult child = rename_item(ctx, src / name, existing, name, params, depth + 1);
        if (child.cross_device) {
            child.result =
                failed(src / name, make_error(ErrorCode::Unsupported,
                                              "mount point inside: " + (src / name).native()));
        }
        for (UndoStep& s : child.result.steps) {
            out.steps.push_back(std::move(s));
        }
        const ItemOutcome& o = child.result.outcome;
        if (o.status != ItemStatus::Done || o.partial) {
            out.outcome.partial = true;
            if (o.error && !out.outcome.error) {
                out.outcome.error = o.error;
            }
            if (o.status == ItemStatus::Failed) {
                ctx.count_failure();
            }
        }
        if (o.status == ItemStatus::Cancelled) {
            out.outcome.status = ItemStatus::Cancelled;
            break;
        }
    }
    return out;
}

RenameResult rename_item(Context& ctx, const Path& src, const Path& dst_dir, std::string dst_name,
                         const MoveParams& params, int depth) {
    RenameResult rr;
    const std::string name = src.filename().native();
    Fd pfd(open_dir_path(src.parent_path()));
    if (!pfd.valid()) {
        rr.result = failed(src, errno_error(errno, "cannot open folder", src.parent_path()));
        return rr;
    }
    struct stat src_st {};
    if (const int rc = lstat_at(pfd.get(), name, src_st); rc != 0) {
        rr.result = failed(src, errno_error(rc, "cannot stat", src));
        return rr;
    }
    Fd dfd(open_dir_path(dst_dir));
    struct stat pst {};
    struct stat dst_st {};
    if (!dfd.valid() || ::fstat(dfd.get(), &dst_st) != 0 || ::fstat(pfd.get(), &pst) != 0) {
        rr.result = failed(src, errno_error(errno, "cannot open destination", dst_dir));
        return rr;
    }
    const bool same_dir = same_inode(pst, dst_st);
    if (same_dir && name == dst_name) {
        rr.result = with_status(src, ItemStatus::Skipped); // a move onto itself
        return rr;
    }
    ctx.set_current(src);
    if (depth > 0 && ctx.inject(src)) {
        rr.result = failed(src, make_error(ErrorCode::IoError, "injected: " + src.native()));
        return rr;
    }
    std::optional<trash::TrashedItem> replaced;
    bool replaced_permanently = false;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (!ctx.checkpoint()) {
            rr.result = with_status(src, ItemStatus::Cancelled);
            return rr;
        }
        int rc = io_fault(IoSite::Rename, src);
        if (rc == 0) {
            rc = rename_noreplace(pfd.get(), name, dfd.get(), dst_name);
            if (rc == EINVAL || rc == ENOSYS) {
                rc = rename_no_overwrite(pfd.get(), name, dfd.get(), dst_name,
                                         !S_ISDIR(src_st.st_mode));
            }
        }
        if (rc == 0) {
            MoveResult& r = rr.result;
            r.outcome.source = src;
            r.outcome.destination = dst_dir / dst_name;
            r.outcome.status = ItemStatus::Done;
            UndoStep step;
            step.kind = params.step_kind;
            step.before = src;
            struct stat st {};
            if (lstat_at(dfd.get(), dst_name, st) == 0) {
                step.after = guard_from(dst_dir / dst_name, st);
            } else {
                step.after.path = dst_dir / dst_name;
            }
            step.replaced = std::move(replaced);
            step.replaced_permanently = replaced_permanently;
            r.steps.push_back(std::move(step));
            ctx.add_items(1);
            return rr;
        }
        if (rc == EXDEV) {
            if (replaced && ctx.trash() != nullptr) {
                (void)ctx.trash()->restore(*replaced, dst_dir / dst_name);
            }
            rr.cross_device = params.allow_cross_device;
            if (!rr.cross_device) {
                rr.result = failed(src, errno_error(rc, "cannot move", src));
            }
            return rr;
        }
        if (rc != EEXIST) {
            if (replaced && ctx.trash() != nullptr) {
                (void)ctx.trash()->restore(*replaced, dst_dir / dst_name);
            }
            rr.result = failed(src, errno_error(rc, "cannot move", src));
            return rr;
        }
        // The name is taken.
        struct stat ex {};
        const int lrc = lstat_at(dfd.get(), dst_name, ex);
        if (lrc == ENOENT) {
            continue;
        }
        if (lrc != 0) {
            rr.result = failed(src, errno_error(lrc, "cannot stat", dst_dir / dst_name));
            return rr;
        }
        if (same_inode(ex, src_st) && same_dir && fs::fold_name(name) == fs::fold_name(dst_name)) {
            // Case-only rename on a case-insensitive filesystem: same FileId, not a conflict.
            const std::string tmp = temp_name_for(dst_name);
            int c = rename_noreplace(pfd.get(), name, pfd.get(), tmp);
            if (c == 0) {
                c = rename_noreplace(pfd.get(), tmp, pfd.get(), dst_name);
                if (c != 0) {
                    (void)rename_noreplace(pfd.get(), tmp, pfd.get(), name);
                }
            }
            if (c != 0) {
                rr.result = failed(src, errno_error(c, "cannot rename", src));
                return rr;
            }
            MoveResult& r = rr.result;
            r.outcome.source = src;
            r.outcome.destination = dst_dir / dst_name;
            UndoStep step;
            step.kind = params.step_kind;
            step.before = src;
            struct stat st {};
            if (lstat_at(dfd.get(), dst_name, st) == 0) {
                step.after = guard_from(dst_dir / dst_name, st);
            }
            r.steps.push_back(std::move(step));
            ctx.add_items(1);
            return rr;
        }
        Placement pl;
        if (params.auto_replace && same_inode(ex, *params.auto_replace) &&
            replace_can_trash(ctx, dst_dir / dst_name)) {
            pl.action = Placement::Action::Replace;
            pl.existing = ex;
        } else {
            ConflictInput in;
            in.source_path = src;
            in.source = &src_st;
            in.dst_dirfd = dfd.get();
            in.dst_dir = dst_dir;
            in.name = dst_name;
            in.existing = ex;
            in.permanent_replace_ok = params.permanent_replace_ok;
            pl = resolve_placement(ctx, in);
        }
        switch (pl.action) {
        case Placement::Action::Skip: rr.result = with_status(src, ItemStatus::Skipped); return rr;
        case Placement::Action::Cancel:
            rr.result = with_status(src, ItemStatus::Cancelled);
            return rr;
        case Placement::Action::Fail: rr.result = failed(src, pl.error); return rr;
        case Placement::Action::KeepBoth: dst_name = pl.name; continue;
        case Placement::Action::Merge:
            rr.result = merge_into(ctx, src, dst_dir / dst_name, params, depth);
            return rr;
        case Placement::Action::Replace:
            if (!pl.permanent) {
                auto item = trash_existing(ctx, dfd.get(), dst_dir, dst_name, pl.existing);
                if (!item) {
                    rr.result = failed(src, item.error());
                    return rr;
                }
                replaced = std::move(*item);
                continue; // the name is free now
            }
            {
                // S8: no trash; overwrite the existing non-directory atomically by rename.
                struct stat now {};
                if (lstat_at(dfd.get(), dst_name, now) != 0 || !same_inode(now, pl.existing)) {
                    continue;
                }
                if (::renameat(pfd.get(), name.c_str(), dfd.get(), dst_name.c_str()) != 0) {
                    rr.result = failed(src, errno_error(errno, "cannot move", src));
                    return rr;
                }
                MoveResult& r = rr.result;
                r.outcome.source = src;
                r.outcome.destination = dst_dir / dst_name;
                UndoStep step;
                step.kind = params.step_kind;
                step.before = src;
                struct stat st {};
                if (lstat_at(dfd.get(), dst_name, st) == 0) {
                    step.after = guard_from(dst_dir / dst_name, st);
                }
                step.replaced_permanently = true;
                r.steps.push_back(std::move(step));
                ctx.add_items(1);
                return rr;
            }
        }
    }
    if (replaced && ctx.trash() != nullptr) {
        (void)ctx.trash()->restore(*replaced, dst_dir / dst_name);
    }
    rr.result = failed(src, make_error(ErrorCode::AlreadyExists, "name keeps being taken"));
    return rr;
}

} // namespace

MoveResult move_item(Context& ctx, const Path& src, const Path& dst_dir,
                     const std::string& dst_name, const MoveParams& params, Tree* prescanned) {
    if (prescanned == nullptr) {
        RenameResult rr = rename_item(ctx, src, dst_dir, dst_name, params, 0);
        if (!rr.cross_device) {
            return std::move(rr.result);
        }
    }
    // Across filesystems (S7): copy, verify, then remove the source.
    std::optional<Tree> local;
    Tree* tree = prescanned;
    if (tree == nullptr) {
        auto scanned = scan_tree(ctx, src, false);
        if (!scanned) {
            if (scanned.error().code == ErrorCode::Cancelled) {
                return with_status(src, ItemStatus::Cancelled);
            }
            return failed(src, scanned.error());
        }
        local = std::move(*scanned);
        tree = &*local;
        const auto avail = available_bytes(dst_dir);
        if (avail && *avail < tree->space) {
            return failed(
                src, make_error(ErrorCode::NoSpace, "not enough space in " + dst_dir.native()));
        }
    }
    TransferParams tp;
    tp.step_kind = params.step_kind;
    tp.move = true;
    tp.dst_dir = dst_dir;
    tp.dst_name = dst_name;
    tp.auto_replace = params.auto_replace;
    tp.permanent_replace_ok = params.permanent_replace_ok;
    tp.inject_top = false; // the caller injected the top-level item already
    TransferResult tr = transfer_tree(ctx, *tree, tp);
    MoveResult out;
    out.outcome = std::move(tr.outcome);
    out.outcome.source = src;
    out.steps = std::move(tr.steps);
    return out;
}

std::optional<Error> check_source(Context& ctx, const Path& src, bool protect, const Path* into) {
    struct stat st {};
    if (const int rc = lstat_path(src, st); rc != 0) {
        return errno_error(rc, "cannot stat", src);
    }
    if (protect) {
        struct stat pst {};
        if (::stat(src.parent_path().c_str(), &pst) == 0 && pst.st_dev != st.st_dev &&
            S_ISDIR(st.st_mode)) {
            return make_error(ErrorCode::PermissionDenied, "a mount point: " + src.native());
        }
    }
    auto canon = canonical_entry(src);
    if (!canon) {
        return canon.error();
    }
    if (protect) {
        const Protection p = protection_of(*canon, ctx.engine().protected_paths);
        if (p != Protection::None ||
            (ctx.trash() != nullptr && ctx.trash()->is_inside_trash(*canon))) {
            return make_error(ErrorCode::PermissionDenied, "protected: " + canon->native());
        }
    }
    if (into != nullptr && S_ISDIR(st.st_mode)) {
        std::unique_ptr<char, decltype(&std::free)> real(::realpath(into->c_str(), nullptr),
                                                         &std::free);
        if (real && is_same_or_inside(Path(real.get()), *canon)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a folder cannot go into itself: " + src.native());
        }
    }
    return std::nullopt;
}

} // namespace navigator::ops::detail
