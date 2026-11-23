// src/ops/simple_ops.cpp: MakeDirectory, CreateSymlink, Trash (S2), Restore and the confirmed
// DeletePermanently (S1a, S3, S4, S5).
#include <algorithm>
#include <cerrno>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/ops.hpp>

#include "conflict.hpp"
#include "context.hpp"
#include "copy.hpp"
#include "kinds.hpp"
#include "move.hpp"
#include "sys.hpp"
#include "tree.hpp"

namespace navigator::ops::detail {
namespace {

constexpr int kMaxAttempts = 6;

[[nodiscard]] PlaceOne failed_one(const Path& src, Error e) {
    PlaceOne p;
    p.outcome.source = src;
    p.outcome.status = ItemStatus::Failed;
    p.outcome.error = std::move(e);
    return p;
}

[[nodiscard]] PlaceOne status_one(const Path& src, ItemStatus s) {
    PlaceOne p;
    p.outcome.source = src;
    p.outcome.status = s;
    if (s == ItemStatus::Cancelled) {
        p.outcome.error = make_error(ErrorCode::Cancelled, "cancelled");
    }
    return p;
}

[[nodiscard]] ItemGuard guard_of(const Path& path) {
    struct stat st {};
    if (lstat_path(path, st) == 0) {
        return guard_from(path, st);
    }
    ItemGuard g;
    g.path = path;
    return g;
}

// Removes exactly the scanned entries of `tree`, bottom up, never following links and never
// entering another filesystem (S1a, S4, S5). Returns the outcome of the top-level item.
ItemOutcome remove_tree(Context& ctx, Tree& tree, const Path& shown) {
    ItemOutcome out;
    out.source = shown;
    Fd root(open_dir_path(tree.parent));
    struct stat rst {};
    if (!root.valid() || ::fstat(root.get(), &rst) != 0 || !same_inode(rst, tree.parent_id)) {
        out.status = ItemStatus::Failed;
        out.error = errno_error(root.valid() ? ESTALE : errno, "cannot open folder", tree.parent);
        return out;
    }
    TreeCursor cur(std::move(root), tree.parent_id, tree, TreeCursor::Side::Source);
    std::optional<Error> first;
    bool removed_any = false;
    bool top_removed = false;
    bool cancelled = false;
    const auto count = static_cast<std::uint32_t>(tree.nodes.size());
    for (std::uint32_t k = count; k-- > 0;) {
        Node& n = tree.nodes[k];
        auto keep = [&](std::optional<Error> why) {
            if (n.parent != kNoParent) {
                tree.nodes[n.parent].kept_child = true;
            }
            if (why) {
                ctx.count_failure();
                if (!first) {
                    first = std::move(why);
                }
            }
        };
        if (!ctx.checkpoint()) {
            cancelled = true;
            break;
        }
        const Path p = node_path(tree, k);
        ctx.set_current(p);
        if (n.kept_child) {
            keep(std::nullopt);
            continue;
        }
        if (ctx.inject(p)) {
            keep(make_error(ErrorCode::IoError, "injected: " + p.native()));
            continue;
        }
        if (n.mount_point) {
            keep(make_error(ErrorCode::Unsupported, "mount point inside: " + p.native()));
            continue;
        }
        if (const int rc = cur.go_to(n.parent); rc != 0) {
            keep(errno_error(rc, "folder changed", p.parent_path()));
            continue;
        }
        struct stat now {};
        const int rc = lstat_at(cur.fd(), n.name, now);
        if (rc == ENOENT) {
            top_removed = top_removed || k == 0; // already gone
            continue;
        }
        if (rc != 0 || !same_inode(now, n.st) ||
            (now.st_mode & S_IFMT) != (n.st.st_mode & S_IFMT)) {
            keep(make_error(ErrorCode::IoError, "changed since the scan: " + p.native()));
            continue;
        }
        if (const int f = io_fault(IoSite::Unlink, p); f != 0) {
            keep(errno_error(f, "cannot delete", p));
            continue;
        }
        const bool dir = S_ISDIR(now.st_mode);
        if (::unlinkat(cur.fd(), n.name.c_str(), dir ? AT_REMOVEDIR : 0) != 0) {
            const int err = errno;
            keep(errno_error(err, n.scan_errno != 0 ? "cannot read" : "cannot delete", p));
            continue;
        }
        removed_any = true;
        ctx.add_items(1);
        if (k == 0) {
            top_removed = true;
        }
    }
    (void)cur.unwind([](std::uint32_t, int) {});
    if (top_removed) {
        out.status = ItemStatus::Done;
    } else if (cancelled) {
        out.status = ItemStatus::Cancelled;
        out.partial = removed_any;
        out.error = make_error(ErrorCode::Cancelled, "cancelled");
        return out;
    } else {
        out.status = ItemStatus::Failed;
        out.partial = removed_any;
    }
    out.error = first;
    return out;
}

} // namespace

Result<std::vector<ItemGuard>> ensure_parents(const Path& dir) {
    std::vector<Path> missing;
    Path cur = dir;
    while (!cur.empty()) {
        struct stat st {};
        const int rc = lstat_path(cur, st);
        if (rc == 0) {
            if (!S_ISDIR(st.st_mode)) {
                return fail(ErrorCode::NotFound, "not a folder: " + cur.native());
            }
            break;
        }
        if (rc != ENOENT) {
            return Unexpected<Error>(errno_error(rc, "cannot stat", cur));
        }
        missing.push_back(cur);
        if (cur == cur.parent_path()) {
            break;
        }
        cur = cur.parent_path();
    }
    std::vector<ItemGuard> created;
    for (const Path& dir_path : std::views::reverse(missing)) {
        if (::mkdir(dir_path.c_str(), 0777) != 0 && errno != EEXIST) {
            return Unexpected<Error>(errno_error(errno, "cannot create folder", dir_path));
        }
        created.push_back(guard_of(dir_path));
    }
    return created;
}

PlaceOne make_directory_at(Context& ctx, const Path& dir, const std::string& name_in) {
    std::string name = name_in;
    Fd dfd(open_dir_path(dir));
    if (!dfd.valid()) {
        return failed_one(dir / name, errno_error(errno, "cannot open folder", dir));
    }
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const Path target = dir / name;
        if (const int f = io_fault(IoSite::Mkdir, target); f != 0) {
            return failed_one(target, errno_error(f, "cannot create folder", target));
        }
        if (::mkdirat(dfd.get(), name.c_str(), 0777) == 0) {
            PlaceOne p;
            p.outcome.source = target;
            p.outcome.destination = target;
            UndoStep step;
            step.kind = OpKind::MakeDirectory;
            step.after = guard_of(target);
            p.steps.push_back(std::move(step));
            ctx.add_items(1);
            return p;
        }
        const int err = errno;
        if (err != EEXIST) {
            return failed_one(target, errno_error(err, "cannot create folder", target));
        }
        struct stat ex {};
        if (lstat_at(dfd.get(), name, ex) != 0) {
            continue;
        }
        ConflictInput in;
        in.source_path = target;
        in.source = nullptr; // a new empty folder
        in.dst_dirfd = dfd.get();
        in.dst_dir = dir;
        in.name = name;
        in.existing = ex;
        in.merge_supported = false;
        in.replace_supported = false;
        const Placement pl = resolve_placement(ctx, in);
        switch (pl.action) {
        case Placement::Action::KeepBoth: name = pl.name; continue;
        case Placement::Action::Cancel: return status_one(target, ItemStatus::Cancelled);
        case Placement::Action::Fail: return failed_one(target, pl.error);
        case Placement::Action::Skip:
        case Placement::Action::Merge:
        case Placement::Action::Replace: return status_one(target, ItemStatus::Skipped);
        }
    }
    return failed_one(dir / name, make_error(ErrorCode::AlreadyExists, "name keeps being taken"));
}

PlaceOne create_symlink_at(Context& ctx, const Path& target, const Path& dir,
                           const std::string& name_in, bool permanent_replace_ok) {
    std::string name = name_in;
    Fd dfd(open_dir_path(dir));
    if (!dfd.valid()) {
        return failed_one(target, errno_error(errno, "cannot open folder", dir));
    }
    struct stat src_st {};
    const bool have_src = lstat_path(target, src_st) == 0;
    std::optional<trash::TrashedItem> replaced;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const Path link = dir / name;
        if (const int f = io_fault(IoSite::CreateTemp, link); f != 0) {
            return failed_one(target, errno_error(f, "cannot create link", link));
        }
        if (::symlinkat(target.c_str(), dfd.get(), name.c_str()) == 0) {
            PlaceOne p;
            p.outcome.source = target;
            p.outcome.destination = link;
            UndoStep step;
            step.kind = OpKind::CreateSymlink;
            step.before = target; // the link's target, needed by Redo
            step.after = guard_of(link);
            step.replaced = std::move(replaced);
            p.steps.push_back(std::move(step));
            ctx.add_items(1);
            return p;
        }
        const int err = errno;
        if (err != EEXIST) {
            if (replaced && ctx.trash() != nullptr) {
                (void)ctx.trash()->restore(*replaced, link);
            }
            return failed_one(target, errno_error(err, "cannot create link", link));
        }
        struct stat ex {};
        if (lstat_at(dfd.get(), name, ex) != 0) {
            continue;
        }
        ConflictInput in;
        in.source_path = target;
        in.dst_dirfd = dfd.get();
        in.dst_dir = dir;
        in.name = name;
        in.existing = ex;
        in.merge_supported = false;
        in.permanent_replace_ok = permanent_replace_ok;
        // The new item is a link (never a folder, whatever it points to; never the same inode).
        struct stat as_link {};
        as_link.st_mode = S_IFLNK | 0777;
        if (have_src) {
            as_link.st_mtim = src_st.st_mtim;
        }
        in.source = &as_link;
        const Placement pl = resolve_placement(ctx, in);
        switch (pl.action) {
        case Placement::Action::KeepBoth: name = pl.name; continue;
        case Placement::Action::Cancel: return status_one(target, ItemStatus::Cancelled);
        case Placement::Action::Fail: return failed_one(target, pl.error);
        case Placement::Action::Skip:
        case Placement::Action::Merge: return status_one(target, ItemStatus::Skipped);
        case Placement::Action::Replace:
            if (!pl.permanent) {
                auto item = trash_existing(ctx, dfd.get(), dir, name, pl.existing);
                if (!item) {
                    return failed_one(target, item.error());
                }
                replaced = std::move(*item);
                continue;
            }
            {
                const std::string tmp = temp_name_for(name);
                if (::symlinkat(target.c_str(), dfd.get(), tmp.c_str()) != 0) {
                    return failed_one(target, errno_error(errno, "cannot create link", link));
                }
                if (::renameat(dfd.get(), tmp.c_str(), dfd.get(), name.c_str()) != 0) {
                    const int rerr = errno;
                    ::unlinkat(dfd.get(), tmp.c_str(), 0);
                    return failed_one(target, errno_error(rerr, "cannot create link", link));
                }
                PlaceOne p;
                p.outcome.source = target;
                p.outcome.destination = link;
                UndoStep step;
                step.kind = OpKind::CreateSymlink;
                step.before = target;
                step.after = guard_of(link);
                step.replaced_permanently = true;
                p.steps.push_back(std::move(step));
                ctx.add_items(1);
                return p;
            }
        }
    }
    return failed_one(target, make_error(ErrorCode::AlreadyExists, "name keeps being taken"));
}

PlaceOne trash_one(Context& ctx, const Path& src) {
    if (ctx.trash() == nullptr) {
        ctx.add_untrashable(src);
        return failed_one(src, make_error(ErrorCode::Unsupported, "no trash available"));
    }
    auto item = ctx.trash()->trash(src);
    if (!item) {
        if (item.error().code == ErrorCode::Unsupported) {
            ctx.add_untrashable(src); // S2: never a permanent fallback
        }
        return failed_one(src, item.error());
    }
    PlaceOne p;
    p.outcome.source = src;
    p.outcome.destination = item->files_path();
    UndoStep step;
    step.kind = OpKind::Trash;
    step.before = src;
    struct stat st {};
    if (lstat_path(item->files_path(), st) == 0) {
        step.after = guard_from(item->files_path(), st);
    } else {
        step.after.path = item->files_path();
        step.after.kind = item->kind;
        step.after.id = item->id;
        step.after.size = item->size;
        step.after.mtime = item->mtime;
    }
    step.trashed = std::move(*item);
    p.steps.push_back(std::move(step));
    ctx.add_items(1);
    return p;
}

PlaceOne restore_one(Context& ctx, const trash::TrashedItem& item, const Path& dest,
                     const std::optional<FileId>& auto_replace) {
    const Path files = item.files_path();
    if (ctx.trash() == nullptr) {
        return failed_one(files, make_error(ErrorCode::Unsupported, "no trash available"));
    }
    auto parents = ensure_parents(dest.parent_path());
    if (!parents) {
        return failed_one(files, parents.error());
    }
    const Path dir = dest.parent_path();
    std::string name = dest.filename().native();
    std::optional<trash::TrashedItem> replaced;
    auto created_parent_steps = [&](PlaceOne& p) {
        // The restore did not happen: the folders made for it stay recorded so undo can rmdir
        // them while empty (S1c).
        for (const ItemGuard& g : *parents) {
            UndoStep s;
            s.kind = OpKind::MakeDirectory;
            s.after = g;
            p.steps.push_back(std::move(s));
        }
    };
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (!ctx.checkpoint()) {
            PlaceOne p = status_one(files, ItemStatus::Cancelled);
            created_parent_steps(p);
            return p;
        }
        const Path target = dir / name;
        auto r = ctx.trash()->restore(item, target);
        if (r) {
            PlaceOne p;
            p.outcome.source = files;
            p.outcome.destination = target;
            UndoStep step;
            step.kind = OpKind::Restore;
            step.before = files;
            step.after = guard_of(target);
            step.trashed = item;
            step.replaced = std::move(replaced);
            step.created_parents = std::move(*parents);
            p.steps.push_back(std::move(step));
            ctx.add_items(1);
            return p;
        }
        const ErrorCode code = r.error().code;
        if (code == ErrorCode::CrossDevice) {
            // Copy out of the trash, verify, erase the trashed copy, forget its info (S7).
            auto tree = scan_tree(ctx, files, false);
            if (!tree) {
                PlaceOne p = failed_one(files, tree.error());
                created_parent_steps(p);
                return p;
            }
            if (const auto avail = available_bytes(dir); avail && *avail < tree->space) {
                PlaceOne p = failed_one(
                    files, make_error(ErrorCode::NoSpace, "not enough space in " + dir.native()));
                created_parent_steps(p);
                return p;
            }
            TransferParams tp;
            tp.step_kind = OpKind::Restore;
            tp.move = true;
            tp.dst_dir = dir;
            tp.dst_name = name;
            tp.auto_replace = auto_replace;
            tp.permanent_replace_ok = false;
            tp.inject_top = false;
            TransferResult tr = transfer_tree(ctx, *tree, tp);
            PlaceOne p;
            p.outcome = std::move(tr.outcome);
            p.outcome.source = files;
            if (tr.source_removed) {
                (void)ctx.trash()->forget(item);
            }
            p.steps = std::move(tr.steps);
            if (!p.steps.empty()) {
                p.steps.front().trashed = item;
                p.steps.front().created_parents = std::move(*parents);
            } else {
                created_parent_steps(p);
            }
            return p;
        }
        if (code != ErrorCode::AlreadyExists) {
            if (replaced) {
                (void)ctx.trash()->restore(*replaced, target);
            }
            PlaceOne p = failed_one(files, r.error());
            created_parent_steps(p);
            return p;
        }
        Fd dfd(open_dir_path(dir));
        struct stat ex {};
        struct stat src_st {};
        if (!dfd.valid() || lstat_at(dfd.get(), name, ex) != 0) {
            continue;
        }
        if (lstat_path(files, src_st) != 0) {
            PlaceOne p = failed_one(files, errno_error(errno, "cannot stat", files));
            created_parent_steps(p);
            return p;
        }
        Placement pl;
        if (auto_replace && same_inode(ex, *auto_replace) && replace_can_trash(ctx, target)) {
            pl.action = Placement::Action::Replace;
            pl.existing = ex;
        } else {
            ConflictInput in;
            in.source_path = files;
            in.source = &src_st;
            in.dst_dirfd = dfd.get();
            in.dst_dir = dir;
            in.name = name;
            in.existing = ex;
            in.merge_supported = false;
            in.permanent_replace_ok = false;
            pl = resolve_placement(ctx, in);
        }
        switch (pl.action) {
        case Placement::Action::KeepBoth: name = pl.name; continue;
        case Placement::Action::Replace: {
            auto t = trash_existing(ctx, dfd.get(), dir, name, pl.existing);
            if (!t) {
                PlaceOne p = failed_one(files, t.error());
                created_parent_steps(p);
                return p;
            }
            replaced = std::move(*t);
            continue;
        }
        case Placement::Action::Cancel: {
            PlaceOne p = status_one(files, ItemStatus::Cancelled);
            created_parent_steps(p);
            return p;
        }
        case Placement::Action::Fail: {
            PlaceOne p = failed_one(files, pl.error);
            created_parent_steps(p);
            return p;
        }
        case Placement::Action::Skip:
        case Placement::Action::Merge: {
            PlaceOne p = status_one(files, ItemStatus::Skipped);
            created_parent_steps(p);
            return p;
        }
        }
    }
    PlaceOne p = failed_one(files, make_error(ErrorCode::AlreadyExists, "name keeps being taken"));
    created_parent_steps(p);
    return p;
}

// ---------------------------------------------------------------------------------------------
// Executors
// ---------------------------------------------------------------------------------------------

Result<void> run_make_directory(Context& ctx) {
    const OpRequest& r = ctx.request();
    ctx.set_top_level_total(1);
    ctx.add_total(1, 0);
    ctx.set_totals_final();
    ctx.add_touched(r.destination);
    PhaseScope working(ctx, OpPhase::Working);
    const Path target = r.destination / r.new_name;
    ctx.set_current(target);
    if (r.options.dry_run) {
        ItemOutcome o;
        o.source = target;
        o.destination = target;
        struct stat st {};
        if (lstat_path(target, st) == 0) {
            o.status = ItemStatus::Conflict;
        }
        ctx.finish_item(std::move(o));
        return ok();
    }
    if (ctx.inject(target)) {
        PlaceOne p =
            failed_one(target, make_error(ErrorCode::IoError, "injected: " + target.native()));
        ctx.finish_item(std::move(p.outcome));
        return ok();
    }
    PlaceOne p = make_directory_at(ctx, r.destination, r.new_name);
    ctx.add_steps(std::move(p.steps));
    ctx.finish_item(std::move(p.outcome));
    return ok();
}

Result<void> run_create_symlink(Context& ctx) {
    const OpRequest& r = ctx.request();
    std::vector<Path> sources;
    for (const Path& s : r.sources) {
        if (std::find(sources.begin(), sources.end(), s) == sources.end()) {
            sources.push_back(s);
        }
    }
    ctx.set_top_level_total(sources.size());
    ctx.add_total(sources.size(), 0);
    ctx.set_totals_final();
    ctx.add_touched(r.destination);
    PhaseScope working(ctx, OpPhase::Working);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const Path& s = sources[i];
        if (!ctx.checkpoint()) {
            for (std::size_t k = i; k < sources.size(); ++k) {
                ctx.finish_item(status_one(sources[k], ItemStatus::Cancelled).outcome);
            }
            break;
        }
        ctx.set_current(s);
        if (r.options.dry_run) {
            ItemOutcome o;
            o.source = s;
            o.destination = r.destination / s.filename();
            struct stat st {};
            if (lstat_path(o.destination, st) == 0) {
                o.status = ItemStatus::Conflict;
            }
            ctx.finish_item(std::move(o));
            continue;
        }
        if (ctx.inject(s)) {
            ctx.finish_item(
                failed_one(s, make_error(ErrorCode::IoError, "injected: " + s.native())).outcome);
            continue;
        }
        PlaceOne p = create_symlink_at(ctx, s, r.destination, s.filename().native(), true);
        ctx.add_steps(std::move(p.steps));
        ctx.finish_item(std::move(p.outcome));
    }
    return ok();
}

Result<void> run_trash(Context& ctx) {
    const OpRequest& r = ctx.request();
    std::vector<Path> sources;
    for (const Path& s : r.sources) {
        if (std::find(sources.begin(), sources.end(), s) == sources.end()) {
            sources.push_back(s);
        }
    }
    ctx.set_top_level_total(sources.size());
    ctx.add_total(sources.size(), 0);
    ctx.set_totals_final();
    PhaseScope working(ctx, OpPhase::Working);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const Path& s = sources[i];
        if (!ctx.checkpoint()) {
            for (std::size_t k = i; k < sources.size(); ++k) {
                ctx.finish_item(status_one(sources[k], ItemStatus::Cancelled).outcome);
            }
            break;
        }
        ctx.set_current(s);
        ctx.add_touched(s.parent_path());
        if (auto e = check_source(ctx, s, true, nullptr)) {
            ctx.finish_item(failed_one(s, *e).outcome);
            continue;
        }
        if (r.options.dry_run) {
            ItemOutcome o;
            o.source = s;
            if (ctx.trash() == nullptr) {
                o.status = ItemStatus::Failed;
                o.error = make_error(ErrorCode::Unsupported, "no trash available");
                ctx.add_untrashable(s);
            } else if (auto d = ctx.trash()->trash_dir_for(s); !d) {
                o.status = ItemStatus::Failed;
                o.error = d.error();
                if (d.error().code == ErrorCode::Unsupported) {
                    ctx.add_untrashable(s);
                }
            }
            ctx.finish_item(std::move(o));
            continue;
        }
        if (ctx.inject(s)) {
            ctx.finish_item(
                failed_one(s, make_error(ErrorCode::IoError, "injected: " + s.native())).outcome);
            continue;
        }
        PlaceOne p = trash_one(ctx, s);
        ctx.add_steps(std::move(p.steps));
        ctx.finish_item(std::move(p.outcome));
    }
    return ok();
}

Result<void> run_restore(Context& ctx) {
    const OpRequest& r = ctx.request();
    ctx.set_top_level_total(r.trashed.size());
    ctx.add_total(r.trashed.size(), 0);
    ctx.set_totals_final();
    PhaseScope working(ctx, OpPhase::Working);
    for (std::size_t i = 0; i < r.trashed.size(); ++i) {
        const trash::TrashedItem& item = r.trashed[i];
        const Path files = item.files_path();
        if (!ctx.checkpoint()) {
            for (std::size_t k = i; k < r.trashed.size(); ++k) {
                ctx.finish_item(
                    status_one(r.trashed[k].files_path(), ItemStatus::Cancelled).outcome);
            }
            break;
        }
        ctx.set_current(files);
        ctx.add_touched(item.original_path.parent_path());
        ctx.add_touched(item.dir.files_dir());
        if (r.options.dry_run) {
            ItemOutcome o;
            o.source = files;
            o.destination = item.original_path;
            struct stat st {};
            if (lstat_path(item.original_path, st) == 0) {
                o.status = ItemStatus::Conflict;
            }
            ctx.finish_item(std::move(o));
            continue;
        }
        if (ctx.inject(files)) {
            ctx.finish_item(
                failed_one(files, make_error(ErrorCode::IoError, "injected: " + files.native()))
                    .outcome);
            continue;
        }
        PlaceOne p = restore_one(ctx, item, item.original_path, std::nullopt);
        ctx.add_steps(std::move(p.steps));
        ctx.finish_item(std::move(p.outcome));
    }
    return ok();
}

Result<void> run_delete(Context& ctx) {
    const OpRequest& r = ctx.request();
    struct Entry {
        Path path;
        Path shown;
        const trash::TrashedItem* trashed = nullptr;
        std::optional<Tree> tree;
        std::optional<Error> pre;
    };
    std::vector<Entry> entries;
    for (const Path& s : r.sources) {
        bool dup = false;
        for (const Entry& e : entries) {
            dup = dup || e.path == s;
        }
        if (!dup) {
            entries.push_back(Entry{s, s, nullptr, std::nullopt, std::nullopt});
        }
    }
    for (const trash::TrashedItem& t : r.trashed) {
        entries.push_back(Entry{t.files_path(), t.files_path(), &t, std::nullopt, std::nullopt});
    }
    ctx.set_top_level_total(entries.size());
    {
        PhaseScope scanning(ctx, OpPhase::Scanning);
        for (Entry& e : entries) {
            if (!ctx.checkpoint()) {
                break;
            }
            if (e.trashed == nullptr) {
                if (auto err = check_source(ctx, e.path, true, nullptr)) {
                    e.pre = std::move(err);
                    continue;
                }
            } else {
                // S15: the trashed item is addressed by its TrashedItem; a stale listing never
                // deletes a different item.
                struct stat st {};
                if (const int rc = lstat_path(e.path, st); rc != 0) {
                    e.pre = errno_error(rc, "cannot stat", e.path);
                    continue;
                }
                const FileId want = e.trashed->id;
                if ((want.device != 0 || want.inode != 0) && !same_inode(st, want)) {
                    e.pre = make_error(ErrorCode::InvalidArgument,
                                       "trashed item changed since listing: " + e.path.native());
                    continue;
                }
            }
            auto t = scan_tree(ctx, e.path, false);
            if (!t) {
                e.pre = t.error();
                continue;
            }
            e.tree = std::move(*t);
        }
        ctx.set_totals_final();
    }
    PhaseScope working(ctx, OpPhase::Working);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Entry& e = entries[i];
        if (!ctx.checkpoint()) {
            for (std::size_t k = i; k < entries.size(); ++k) {
                ctx.finish_item(status_one(entries[k].shown, ItemStatus::Cancelled).outcome);
            }
            break;
        }
        ctx.add_touched(e.path.parent_path());
        if (e.pre) {
            ctx.finish_item(failed_one(e.shown, *e.pre).outcome);
            continue;
        }
        if (r.options.dry_run) {
            ItemOutcome o;
            o.source = e.shown;
            ctx.finish_item(std::move(o));
            continue;
        }
        ItemOutcome o = remove_tree(ctx, *e.tree, e.shown);
        if (e.trashed != nullptr && o.status == ItemStatus::Done && ctx.trash() != nullptr) {
            if (auto f = ctx.trash()->forget(*e.trashed);
                !f && f.error().code != ErrorCode::NotFound) {
                o.error = f.error();
            }
        }
        const bool stop = o.status == ItemStatus::Cancelled;
        ctx.finish_item(std::move(o));
        if (stop) {
            for (std::size_t k = i + 1; k < entries.size(); ++k) {
                ctx.finish_item(status_one(entries[k].shown, ItemStatus::Cancelled).outcome);
            }
            break;
        }
    }
    return ok();
}

} // namespace navigator::ops::detail
