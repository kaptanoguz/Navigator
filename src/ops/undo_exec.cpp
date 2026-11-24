// src/ops/undo_exec.cpp: OpKind::Undo and OpKind::Redo (S12).
//
// Undo runs the inverse of each step, newest first, after checking the step's ItemGuard; it never
// deletes (created items go to the trash, folders Navigator made are rmdir'ed only while empty)
// and never overwrites (a taken name is a conflict; Replace only through the trash).
//
// Its result record is the FORWARD record again (kind and step kinds unchanged), describing what
// Redo has to repeat. In such a redo-side record the guard fields describe the item where it is
// now (Move / Rename / Trash: at `before`; Restore: the trashed item), and `after.path` /
// `before` keep the forward meaning, so "Redo = run the record again" (ops.hpp) needs no other
// state. Redo's own result is an ordinary forward record (guards at after.path) for the undo
// side.
#include <cerrno>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
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

[[nodiscard]] ItemOutcome outcome(const Path& src, ItemStatus s, std::optional<Error> e = {}) {
    ItemOutcome o;
    o.source = src;
    o.status = s;
    o.error = std::move(e);
    return o;
}

// Changed outcome when the item at `at` is not the one `g` describes (S12).
[[nodiscard]] std::optional<ItemOutcome> verify(const Path& at, const ItemGuard& g) {
    struct stat st {};
    const int rc = lstat_path(at, st);
    if (rc != 0) {
        return outcome(at, ItemStatus::Changed, errno_error(rc, "no longer there", at));
    }
    if (!matches_guard(st, g)) {
        return outcome(at, ItemStatus::Changed,
                       make_error(ErrorCode::InvalidArgument, "changed since: " + at.native()));
    }
    return std::nullopt;
}

[[nodiscard]] ItemGuard guard_at(const Path& path) {
    ItemGuard g;
    struct stat st {};
    if (lstat_path(path, st) == 0) {
        g = guard_from(path, st);
    } else {
        g.path = path;
    }
    return g;
}

// Redo-side step from a step that moved an item back (ps.before -> ps.after.path).
[[nodiscard]] UndoStep back_step(OpKind kind, const UndoStep& ps) {
    UndoStep r;
    r.kind = kind;
    r.before = ps.after.path; // where the item is now
    r.after = ps.after;       // its identity there ...
    r.after.path = ps.before; // ... and where Redo moves it
    return r;
}

[[nodiscard]] bool restore_replaced(Context& ctx, const UndoStep& s, ItemOutcome& o) {
    if (!s.replaced || ctx.trash() == nullptr) {
        return false;
    }
    if (auto r = ctx.trash()->restore(*s.replaced, s.after.path); !r) {
        o.partial = true;
        if (!o.error) {
            o.error = r.error();
        }
        return false;
    }
    return true;
}

void undo_step(Context& ctx, const UndoStep& s, std::vector<UndoStep>& redo) {
    const Path& at = s.after.path;
    switch (s.kind) {
    case OpKind::Copy:
    case OpKind::CreateSymlink: {
        if (auto changed = verify(at, s.after)) {
            ctx.finish_item(std::move(*changed));
            return;
        }
        if (auto e = check_source(ctx, at, true, nullptr)) {
            ctx.finish_item(outcome(at, ItemStatus::Failed, std::move(e)));
            return;
        }
        if (ctx.trash() == nullptr) {
            ctx.finish_item(outcome(at, ItemStatus::Failed,
                                    make_error(ErrorCode::Unsupported, "no trash available")));
            return;
        }
        auto t = ctx.trash()->trash(at); // S12: undo never deletes
        if (!t) {
            ctx.finish_item(outcome(at, ItemStatus::Failed, t.error()));
            return;
        }
        ItemOutcome o = outcome(at, ItemStatus::Done);
        o.destination = t->files_path();
        UndoStep r = s;
        if (s.replaced && !restore_replaced(ctx, s, o)) {
            r.replaced.reset();
        }
        redo.push_back(std::move(r));
        ctx.finish_item(std::move(o));
        return;
    }
    case OpKind::Move:
    case OpKind::Rename: {
        if (auto changed = verify(at, s.after)) {
            ctx.finish_item(std::move(*changed));
            return;
        }
        const Path back_dir = s.before.parent_path();
        if (auto e = check_source(ctx, at, true, &back_dir)) {
            ctx.finish_item(outcome(at, ItemStatus::Failed, std::move(e)));
            return;
        }
        MoveParams mp;
        mp.step_kind = s.kind;
        mp.permanent_replace_ok = false;
        mp.allow_cross_device = s.kind == OpKind::Move;
        MoveResult mr = move_item(ctx, at, back_dir, s.before.filename().native(), mp, nullptr);
        if (mr.outcome.status == ItemStatus::Done && !mr.steps.empty()) {
            const bool replaced_back = s.replaced && restore_replaced(ctx, s, mr.outcome);
            for (const UndoStep& ps : mr.steps) {
                redo.push_back(back_step(s.kind, ps));
            }
            if (replaced_back && mr.steps.size() == 1) {
                redo.back().replaced = s.replaced;
            }
        } else {
            for (const UndoStep& ps : mr.steps) { // partial merge: what did move back
                redo.push_back(back_step(s.kind, ps));
            }
        }
        ctx.finish_item(std::move(mr.outcome));
        return;
    }
    case OpKind::MakeDirectory: {
        if (auto changed = verify(at, s.after)) {
            ctx.finish_item(std::move(*changed));
            return;
        }
        // S1c: only while empty (rmdir refuses otherwise).
        if (::rmdir(at.c_str()) != 0) {
            const int err = errno;
            ctx.finish_item(outcome(
                at, err == ENOTEMPTY || err == EEXIST ? ItemStatus::Changed : ItemStatus::Failed,
                errno_error(err, "cannot remove folder", at)));
            return;
        }
        redo.push_back(s);
        ctx.finish_item(outcome(at, ItemStatus::Done));
        return;
    }
    case OpKind::Trash: {
        if (!s.trashed) {
            ctx.finish_item(outcome(at, ItemStatus::Failed,
                                    make_error(ErrorCode::InvalidArgument, "no trash item")));
            return;
        }
        if (auto changed = verify(s.trashed->files_path(), s.after)) {
            ctx.finish_item(std::move(*changed));
            return;
        }
        PlaceOne p = restore_one(ctx, *s.trashed, s.before, std::nullopt);
        if (p.outcome.status == ItemStatus::Done) {
            UndoStep r;
            r.kind = OpKind::Trash;
            r.before = p.outcome.destination;
            r.after = guard_at(p.outcome.destination);
            r.trashed = s.trashed; // stale after the restore; Redo trashes `before` again
            redo.push_back(std::move(r));
        }
        ctx.finish_item(std::move(p.outcome));
        return;
    }
    case OpKind::Restore: {
        if (auto changed = verify(at, s.after)) {
            ctx.finish_item(std::move(*changed));
            return;
        }
        if (auto e = check_source(ctx, at, true, nullptr)) {
            ctx.finish_item(outcome(at, ItemStatus::Failed, std::move(e)));
            return;
        }
        if (ctx.trash() == nullptr) {
            ctx.finish_item(outcome(at, ItemStatus::Failed,
                                    make_error(ErrorCode::Unsupported, "no trash available")));
            return;
        }
        auto t = ctx.trash()->trash(at);
        if (!t) {
            ctx.finish_item(outcome(at, ItemStatus::Failed, t.error()));
            return;
        }
        ItemOutcome o = outcome(at, ItemStatus::Done);
        o.destination = t->files_path();
        if (s.replaced) {
            (void)restore_replaced(ctx, s, o);
        }
        // Folders the restore created: innermost first, only while empty and still ours (S1c).
        for (const ItemGuard& g : std::views::reverse(s.created_parents)) {
            struct stat st {};
            if (lstat_path(g.path, st) == 0 && S_ISDIR(st.st_mode) && same_inode(st, g.id)) {
                (void)::rmdir(g.path.c_str());
            }
        }
        UndoStep r;
        r.kind = OpKind::Restore;
        r.before = t->files_path();
        r.after = guard_at(t->files_path());
        r.after.path = at;
        r.trashed = std::move(*t);
        redo.push_back(std::move(r));
        ctx.finish_item(std::move(o));
        return;
    }
    case OpKind::DeletePermanently:
    case OpKind::BatchRename:
    case OpKind::Undo:
    case OpKind::Redo: break;
    }
    ctx.finish_item(
        outcome(at, ItemStatus::Failed, make_error(ErrorCode::InvalidArgument, "not undoable")));
}

void redo_step(Context& ctx, const UndoStep& s) {
    const Path dir = s.after.path.parent_path();
    const std::string name = s.after.path.filename().native();
    switch (s.kind) {
    case OpKind::Copy: {
        auto tree = scan_tree(ctx, s.before, false);
        if (!tree) {
            ctx.finish_item(outcome(s.before, ItemStatus::Failed, tree.error()));
            return;
        }
        if (const auto avail = available_bytes(dir); avail && *avail < tree->space) {
            ctx.finish_item(
                outcome(s.before, ItemStatus::Failed,
                        make_error(ErrorCode::NoSpace, "not enough space in " + dir.native())));
            return;
        }
        TransferParams tp;
        tp.step_kind = OpKind::Copy;
        tp.dst_dir = dir;
        tp.dst_name = name;
        if (s.replaced) {
            tp.auto_replace = s.replaced->id;
        }
        tp.permanent_replace_ok = false;
        TransferResult tr = transfer_tree(ctx, *tree, tp);
        tr.outcome.source = s.before;
        ctx.add_steps(std::move(tr.steps));
        ctx.finish_item(std::move(tr.outcome));
        return;
    }
    case OpKind::Move:
    case OpKind::Rename: {
        if (auto changed = verify(s.before, s.after)) {
            ctx.finish_item(std::move(*changed));
            return;
        }
        if (auto e = check_source(ctx, s.before, true, &dir)) {
            ctx.finish_item(outcome(s.before, ItemStatus::Failed, std::move(e)));
            return;
        }
        MoveParams mp;
        mp.step_kind = s.kind;
        mp.permanent_replace_ok = false;
        mp.allow_cross_device = s.kind == OpKind::Move;
        if (s.replaced) {
            mp.auto_replace = s.replaced->id;
        }
        MoveResult mr = move_item(ctx, s.before, dir, name, mp, nullptr);
        ctx.add_steps(std::move(mr.steps));
        ctx.finish_item(std::move(mr.outcome));
        return;
    }
    case OpKind::MakeDirectory: {
        PlaceOne p = make_directory_at(ctx, dir, name);
        ctx.add_steps(std::move(p.steps));
        ctx.finish_item(std::move(p.outcome));
        return;
    }
    case OpKind::CreateSymlink: {
        PlaceOne p = create_symlink_at(ctx, s.before, dir, name, false);
        ctx.add_steps(std::move(p.steps));
        ctx.finish_item(std::move(p.outcome));
        return;
    }
    case OpKind::Trash: {
        if (auto changed = verify(s.before, s.after)) {
            ctx.finish_item(std::move(*changed));
            return;
        }
        if (auto e = check_source(ctx, s.before, true, nullptr)) {
            ctx.finish_item(outcome(s.before, ItemStatus::Failed, std::move(e)));
            return;
        }
        PlaceOne p = trash_one(ctx, s.before);
        ctx.add_steps(std::move(p.steps));
        ctx.finish_item(std::move(p.outcome));
        return;
    }
    case OpKind::Restore: {
        if (!s.trashed) {
            ctx.finish_item(outcome(s.before, ItemStatus::Failed,
                                    make_error(ErrorCode::InvalidArgument, "no trash item")));
            return;
        }
        if (auto changed = verify(s.trashed->files_path(), s.after)) {
            ctx.finish_item(std::move(*changed));
            return;
        }
        std::optional<FileId> auto_replace;
        if (s.replaced) {
            auto_replace = s.replaced->id;
        }
        PlaceOne p = restore_one(ctx, *s.trashed, s.after.path, auto_replace);
        ctx.add_steps(std::move(p.steps));
        ctx.finish_item(std::move(p.outcome));
        return;
    }
    case OpKind::DeletePermanently:
    case OpKind::BatchRename:
    case OpKind::Undo:
    case OpKind::Redo: break;
    }
    ctx.finish_item(outcome(s.after.path, ItemStatus::Failed,
                            make_error(ErrorCode::InvalidArgument, "not redoable")));
}

[[nodiscard]] Path step_subject(const UndoStep& s, bool undo) {
    if (undo) {
        return s.kind == OpKind::Trash && s.trashed ? s.trashed->files_path() : s.after.path;
    }
    if (s.kind == OpKind::Restore && s.trashed) {
        return s.trashed->files_path();
    }
    // MakeDirectory has no `before`; CreateSymlink's `before` is the link target.
    return s.before.empty() || s.kind == OpKind::CreateSymlink ? s.after.path : s.before;
}

// Staged undo/redo for OpKind::BatchRename records so cyclic renames (e.g. a->b, b->a) invert
// cleanly without false conflicts while still verifying every ItemGuard (S12).
Result<void> run_batch_rename_undo_redo(Context& ctx, bool is_undo) {
    const UndoRecord& rec = *ctx.request().record;
    const std::size_t n = rec.steps.size();
    ctx.set_top_level_total(n);
    ctx.add_total(n, 0);
    ctx.set_totals_final();
    PhaseScope working(ctx, OpPhase::Working);

    // Pair (from_path, to_path, expected_guard) for each step.
    struct Entry {
        const UndoStep* step = nullptr;
        Path from;
        Path to;
        bool valid = true;
        bool staged = false;
        std::string temp_name;
    };
    std::vector<Entry> entries;
    entries.reserve(n);
    std::unordered_set<std::string> targets;
    targets.reserve(n);
    for (const UndoStep& s : rec.steps) {
        Entry e;
        e.step = &s;
        e.from = is_undo ? s.after.path : s.before;
        e.to = is_undo ? s.before : s.after.path;
        ctx.add_touched(e.from.parent_path());
        ctx.add_touched(e.to.parent_path());
        targets.insert(e.to.native());
        entries.push_back(std::move(e));
    }

    if (ctx.request().options.dry_run) {
        for (const Entry& e : entries) {
            if (auto changed = verify(e.from, e.step->after)) {
                ctx.finish_item(std::move(*changed));
            } else {
                ctx.finish_item(outcome(e.from, ItemStatus::Done));
            }
        }
        return ok();
    }

    // Phase 1: verify guards & stage entries whose `from` is another entry's `to`.
    std::vector<std::optional<ItemOutcome>> early_outcome(n);
    for (std::size_t i = 0; i < n; ++i) {
        Entry& e = entries[i];
        if (auto changed = verify(e.from, e.step->after)) {
            e.valid = false;
            early_outcome[i] = std::move(*changed);
            continue;
        }
        const Path to_dir = e.to.parent_path();
        if (auto err = check_source(ctx, e.from, true, &to_dir)) {
            e.valid = false;
            early_outcome[i] = outcome(e.from, ItemStatus::Failed, std::move(err));
            continue;
        }
        if (targets.contains(e.from.native())) {
            const Fd dir(open_dir_path(e.from.parent_path()));
            if (!dir.valid()) {
                e.valid = false;
                early_outcome[i] =
                    outcome(e.from, ItemStatus::Failed, errno_error(errno, "open dir", e.from));
                continue;
            }
            std::string tmp = temp_name_for(e.from.filename().native());
            const int rc = rename_no_overwrite(dir.get(), e.from.filename().native(), dir.get(),
                                               tmp, false);
            if (rc != 0) {
                e.valid = false;
                early_outcome[i] =
                    outcome(e.from, ItemStatus::Failed, errno_error(rc, "stage undo", e.from));
                continue;
            }
            e.staged = true;
            e.temp_name = std::move(tmp);
        }
    }

    // Phase 2: commit each valid entry to `to`.
    std::vector<UndoStep> next_steps;
    next_steps.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        Entry& e = entries[i];
        if (!e.valid) {
            ctx.finish_item(std::move(*early_outcome[i]));
            continue;
        }
        ctx.set_current(e.from);
        const Path parent = e.to.parent_path();
        const Fd dir(open_dir_path(parent));
        if (!dir.valid()) {
            ctx.finish_item(
                outcome(e.from, ItemStatus::Failed, errno_error(errno, "open dir", parent)));
            continue;
        }
        const std::string src_name = e.staged ? e.temp_name : e.from.filename().native();
        const int rc =
            rename_no_overwrite(dir.get(), src_name, dir.get(), e.to.filename().native(), false);
        if (rc != 0) {
            if (e.staged) {
                (void)rename_no_overwrite(dir.get(), e.temp_name, dir.get(),
                                          e.from.filename().native(), false);
            }
            ctx.finish_item(
                outcome(e.from, ItemStatus::Failed, errno_error(rc, "rename", e.from)));
            continue;
        }
        UndoStep ns;
        ns.kind = OpKind::Rename;
        ns.before = e.step->before;
        ns.after = guard_at(e.to);
        if (is_undo) {
            // Redo-side step: guard matches the item now at `before`, target path is `after.path`.
            ns.after.path = e.step->after.path;
        }
        next_steps.push_back(std::move(ns));
        ItemOutcome o = outcome(e.from, ItemStatus::Done);
        o.destination = e.to;
        ctx.finish_item(std::move(o));
    }
    ctx.add_steps(std::move(next_steps));
    return ok();
}

} // namespace

Result<void> run_undo(Context& ctx) {
    const UndoRecord& rec = *ctx.request().record;
    if (rec.kind == OpKind::BatchRename) {
        return run_batch_rename_undo_redo(ctx, true);
    }
    ctx.set_top_level_total(rec.steps.size());
    ctx.add_total(rec.steps.size(), 0);
    ctx.set_totals_final();
    PhaseScope working(ctx, OpPhase::Working);
    std::vector<UndoStep> redo;
    for (auto it = rec.steps.rbegin(); it != rec.steps.rend(); ++it) {
        const Path subject = step_subject(*it, true);
        if (!ctx.checkpoint()) {
            for (auto rest = it; rest != rec.steps.rend(); ++rest) {
                ctx.finish_item(outcome(step_subject(*rest, true), ItemStatus::Cancelled,
                                        make_error(ErrorCode::Cancelled, "cancelled")));
            }
            break;
        }
        ctx.set_current(subject);
        ctx.add_touched(it->after.path.parent_path());
        if (!it->before.empty() && it->kind != OpKind::CreateSymlink) {
            ctx.add_touched(it->before.parent_path());
        }
        if (ctx.request().options.dry_run) {
            const Path at = step_subject(*it, true);
            if (auto changed = verify(at, it->after)) {
                ctx.finish_item(std::move(*changed));
            } else {
                ctx.finish_item(outcome(subject, ItemStatus::Done));
            }
            continue;
        }
        if (ctx.inject(subject)) {
            ctx.finish_item(
                outcome(subject, ItemStatus::Failed,
                        make_error(ErrorCode::IoError, "injected: " + subject.native())));
            continue;
        }
        undo_step(ctx, *it, redo);
    }
    std::vector<UndoStep> forward(std::make_move_iterator(redo.rbegin()),
                                  std::make_move_iterator(redo.rend()));
    ctx.add_steps(std::move(forward));
    return ok();
}

Result<void> run_redo(Context& ctx) {
    const UndoRecord& rec = *ctx.request().record;
    if (rec.kind == OpKind::BatchRename) {
        return run_batch_rename_undo_redo(ctx, false);
    }
    ctx.set_top_level_total(rec.steps.size());
    ctx.add_total(rec.steps.size(), 0);
    ctx.set_totals_final();
    PhaseScope working(ctx, OpPhase::Working);
    for (auto it = rec.steps.begin(); it != rec.steps.end(); ++it) {
        const Path subject = step_subject(*it, false);
        if (!ctx.checkpoint()) {
            for (auto rest = it; rest != rec.steps.end(); ++rest) {
                ctx.finish_item(outcome(step_subject(*rest, false), ItemStatus::Cancelled,
                                        make_error(ErrorCode::Cancelled, "cancelled")));
            }
            break;
        }
        ctx.set_current(subject);
        ctx.add_touched(it->after.path.parent_path());
        if (ctx.request().options.dry_run) {
            ctx.finish_item(outcome(subject, ItemStatus::Done));
            continue;
        }
        if (ctx.inject(subject)) {
            ctx.finish_item(
                outcome(subject, ItemStatus::Failed,
                        make_error(ErrorCode::IoError, "injected: " + subject.native())));
            continue;
        }
        redo_step(ctx, *it);
    }
    return ok();
}

} // namespace navigator::ops::detail
