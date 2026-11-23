// src/ops/engine.cpp: run_operation (validation S3/S13/S15, dispatch) and the Copy, Move and
// Rename executors.
#include <algorithm>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include <navigator/ops.hpp>

#include "context.hpp"
#include "copy.hpp"
#include "internal.hpp"
#include "kinds.hpp"
#include "move.hpp"
#include "sys.hpp"
#include "tree.hpp"

namespace navigator::ops::detail {
namespace {

[[nodiscard]] Result<void> invalid(std::string msg) {
    return fail(ErrorCode::InvalidArgument, std::move(msg));
}

[[nodiscard]] bool needs_sources(OpKind k) noexcept {
    return k == OpKind::Copy || k == OpKind::Move || k == OpKind::Rename ||
           k == OpKind::CreateSymlink || k == OpKind::Trash;
}

[[nodiscard]] bool protects_sources(OpKind k) noexcept {
    return k == OpKind::Move || k == OpKind::Rename || k == OpKind::Trash ||
           k == OpKind::DeletePermanently;
}

[[nodiscard]] std::vector<Path> dedup(const std::vector<Path>& in) {
    std::vector<Path> out;
    out.reserve(in.size());
    for (const Path& p : in) {
        if (std::find(out.begin(), out.end(), p) == out.end()) {
            out.push_back(p);
        }
    }
    return out;
}

[[nodiscard]] bool same_dir(const Path& a, const Path& b) {
    struct stat sa {};
    struct stat sb {};
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && same_inode(sa, sb);
}

void finish_remaining_cancelled(Context& ctx, const std::vector<Path>& items, std::size_t from) {
    for (std::size_t i = from; i < items.size(); ++i) {
        ItemOutcome o;
        o.source = items[i];
        o.status = ItemStatus::Cancelled;
        o.error = make_error(ErrorCode::Cancelled, "cancelled");
        ctx.finish_item(std::move(o));
    }
}

[[nodiscard]] ItemOutcome failed_item(const Path& src, Error e) {
    ItemOutcome o;
    o.source = src;
    o.status = ItemStatus::Failed;
    o.error = std::move(e);
    return o;
}

[[nodiscard]] Result<void> validate_record(const UndoRecord& rec) {
    if (rec.kind == OpKind::Undo || rec.kind == OpKind::Redo ||
        rec.kind == OpKind::DeletePermanently) {
        return invalid("record kind is not undoable");
    }
    for (const UndoStep& s : rec.steps) {
        if (s.kind == OpKind::Undo || s.kind == OpKind::Redo ||
            s.kind == OpKind::DeletePermanently) {
            return invalid("step kind is not undoable");
        }
        if (!s.before.empty() && s.kind != OpKind::CreateSymlink && !is_normal_absolute(s.before)) {
            return invalid("step path is not absolute and normal: " + s.before.native());
        }
        if (!s.after.path.empty() && !is_normal_absolute(s.after.path)) {
            return invalid("step path is not absolute and normal: " + s.after.path.native());
        }
    }
    return ok();
}

} // namespace

Result<void> validate_request(const OpRequest& request, const ProtectedPaths& protected_paths,
                              const trash::ITrash* trash) {
    const OpRequest& r = request;
    const ProtectedPaths& pp = protected_paths;
    // S3: permanent delete only through the confirmation sheet.
    if (r.kind == OpKind::DeletePermanently && !r.confirmed) {
        return invalid("permanent delete requires confirmation");
    }
    if (needs_sources(r.kind) && r.sources.empty()) {
        return invalid("no sources");
    }
    if (r.kind == OpKind::Rename && r.sources.size() != 1) {
        return invalid("rename takes exactly one source");
    }
    if (r.kind == OpKind::Restore && r.trashed.empty()) {
        return invalid("no trashed items");
    }
    if (r.kind == OpKind::DeletePermanently && r.sources.empty() && r.trashed.empty()) {
        return invalid("nothing to delete");
    }
    if ((r.kind == OpKind::Undo || r.kind == OpKind::Redo)) {
        if (!r.record) {
            return invalid("undo / redo without a record");
        }
        return validate_record(*r.record);
    }
    for (const Path& s : r.sources) {
        if (!is_normal_absolute(s)) {
            return invalid("source path is not absolute and normal: " + s.native());
        }
        if (protects_sources(r.kind)) {
            const Protection p = protection_of(s, pp);
            if (p != Protection::None || (trash != nullptr && trash->is_inside_trash(s))) {
                return fail(ErrorCode::PermissionDenied, "protected source: " + s.native());
            }
        }
    }
    for (const trash::TrashedItem& t : r.trashed) {
        if (t.name.empty() || validate_new_name(t.name) != NameError::None ||
            !is_normal_absolute(t.dir.root)) {
            return invalid("invalid trashed item");
        }
        if (r.kind == OpKind::Restore && !is_normal_absolute(t.original_path)) {
            return invalid("trashed item without an absolute original path");
        }
    }
    const bool has_dest = r.kind == OpKind::Copy || r.kind == OpKind::Move ||
                          r.kind == OpKind::CreateSymlink || r.kind == OpKind::MakeDirectory;
    if (has_dest && !is_normal_absolute(r.destination)) {
        return invalid("destination is not absolute and normal: " + r.destination.native());
    }
    if ((r.kind == OpKind::Rename || r.kind == OpKind::MakeDirectory) &&
        validate_new_name(r.new_name) != NameError::None) {
        return invalid("invalid new name");
    }
    if (r.kind == OpKind::Copy || r.kind == OpKind::Move) {
        for (const Path& s : r.sources) {
            if (is_same_or_inside(r.destination, s)) {
                return invalid("a folder cannot go into itself: " + s.native());
            }
        }
    }
    return ok();
}

std::vector<Path> request_paths(const OpRequest& r) {
    std::vector<Path> out = r.sources;
    switch (r.kind) {
    case OpKind::Copy:
    case OpKind::Move:
    case OpKind::CreateSymlink: out.push_back(r.destination); break;
    case OpKind::MakeDirectory: out.push_back(r.destination / r.new_name); break;
    case OpKind::Rename:
        if (!r.sources.empty()) {
            out.push_back(r.sources.front().parent_path() / r.new_name);
        }
        break;
    case OpKind::Trash:
    case OpKind::Restore:
    case OpKind::DeletePermanently:
    case OpKind::Undo:
    case OpKind::Redo: break;
    }
    for (const trash::TrashedItem& t : r.trashed) {
        out.push_back(t.files_path());
        if (!t.original_path.empty()) {
            out.push_back(t.original_path);
        }
    }
    if (r.record) {
        for (const UndoStep& s : r.record->steps) {
            if (!s.before.empty() && s.before.is_absolute()) {
                out.push_back(s.before);
            }
            if (!s.after.path.empty()) {
                out.push_back(s.after.path);
            }
            if (s.trashed) {
                out.push_back(s.trashed->files_path());
            }
            if (s.replaced) {
                out.push_back(s.replaced->files_path());
            }
        }
    }
    return out;
}

OpResult failed_result(const OpRequest& r, const Error& error, bool cancelled) {
    OpResult res;
    res.kind = r.kind;
    res.origin = r.origin;
    res.cancelled = cancelled;
    std::vector<Path> items = dedup(r.sources);
    for (const trash::TrashedItem& t : r.trashed) {
        items.push_back(t.files_path());
    }
    if (r.record) {
        for (const UndoStep& s : std::views::reverse(r.record->steps)) {
            items.push_back(s.after.path);
        }
    }
    if (r.kind == OpKind::MakeDirectory) {
        items.push_back(r.destination / r.new_name);
    }
    for (const Path& p : items) {
        ItemOutcome o;
        o.source = p;
        o.status = cancelled ? ItemStatus::Cancelled : ItemStatus::Failed;
        o.error = error;
        res.items.push_back(std::move(o));
    }
    if (cancelled) {
        res.cancelled = true;
    } else {
        res.failed = res.items.size();
    }
    return res;
}

// ---------------------------------------------------------------------------------------------
// Copy
// ---------------------------------------------------------------------------------------------

Result<void> run_copy(Context& ctx) {
    const OpRequest& r = ctx.request();
    const std::vector<Path> sources = dedup(r.sources);
    ctx.set_top_level_total(sources.size());
    const Path& dest = r.destination;
    ctx.add_touched(dest);

    std::vector<std::unique_ptr<Tree>> trees(sources.size());
    std::vector<std::optional<Error>> pre(sources.size());
    std::uint64_t space = 0;
    {
        PhaseScope scanning(ctx, OpPhase::Scanning);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (!ctx.checkpoint()) {
                break;
            }
            if (auto e = check_source(ctx, sources[i], false, &dest)) {
                pre[i] = std::move(e);
                continue;
            }
            auto t = scan_tree(ctx, sources[i], r.options.dereference_top_level);
            if (!t) {
                pre[i] = t.error();
                continue;
            }
            space += t->space;
            trees[i] = std::make_unique<Tree>(std::move(*t));
        }
        ctx.set_totals_final();
    }
    if (ctx.cancelled()) {
        finish_remaining_cancelled(ctx, sources, 0);
        return ok();
    }
    // S14: nothing is written when it cannot fit.
    if (const auto avail = available_bytes(dest); avail && *avail < space) {
        return fail(ErrorCode::NoSpace, "not enough space in " + dest.native() + ": " +
                                            std::to_string(space) + " bytes needed, " +
                                            std::to_string(*avail) + " available");
    }
    PhaseScope working(ctx, OpPhase::Working);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!ctx.checkpoint()) {
            finish_remaining_cancelled(ctx, sources, i);
            break;
        }
        const Path& src = sources[i];
        if (pre[i]) {
            ctx.finish_item(failed_item(src, pre[i].value_or(Error{})));
            continue;
        }
        const bool own_folder = same_dir(src.parent_path(), dest);
        if (r.options.dry_run) {
            ItemOutcome o;
            o.source = src;
            o.destination = dest / src.filename();
            struct stat st {};
            if (!own_folder && lstat_path(o.destination, st) == 0) {
                o.status = ItemStatus::Conflict;
            }
            ctx.finish_item(std::move(o));
            continue;
        }
        ctx.add_touched(src.parent_path());
        TransferParams tp;
        tp.step_kind = OpKind::Copy;
        tp.force_keep_both = own_folder;
        tp.dst_dir = dest;
        tp.dst_name = src.filename().native();
        TransferResult tr = transfer_tree(ctx, *trees[i], tp);
        tr.outcome.source = src;
        ctx.add_steps(std::move(tr.steps));
        const bool stop = tr.outcome.status == ItemStatus::Cancelled;
        ctx.finish_item(std::move(tr.outcome));
        if (stop) {
            finish_remaining_cancelled(ctx, sources, i + 1);
            break;
        }
    }
    return ok();
}

// ---------------------------------------------------------------------------------------------
// Move
// ---------------------------------------------------------------------------------------------

Result<void> run_move(Context& ctx) {
    const OpRequest& r = ctx.request();
    const std::vector<Path> sources = dedup(r.sources);
    ctx.set_top_level_total(sources.size());
    const Path& dest = r.destination;
    ctx.add_touched(dest);
    struct stat dst_st {};
    const bool dest_ok = ::stat(dest.c_str(), &dst_st) == 0 && S_ISDIR(dst_st.st_mode);

    std::vector<std::unique_ptr<Tree>> trees(sources.size());
    std::vector<std::optional<Error>> pre(sources.size());
    std::uint64_t space = 0;
    {
        PhaseScope scanning(ctx, OpPhase::Scanning);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (!ctx.checkpoint()) {
                break;
            }
            if (!dest_ok) {
                pre[i] =
                    make_error(ErrorCode::NotFound, "destination folder missing: " + dest.native());
                continue;
            }
            if (auto e = check_source(ctx, sources[i], true, &dest)) {
                pre[i] = std::move(e);
                continue;
            }
            struct stat st {};
            if (lstat_path(sources[i], st) == 0 && st.st_dev != dst_st.st_dev) {
                auto t = scan_tree(ctx, sources[i], false); // cross-filesystem (S7, S14)
                if (!t) {
                    pre[i] = t.error();
                    continue;
                }
                space += t->space;
                trees[i] = std::make_unique<Tree>(std::move(*t));
            } else {
                ctx.add_total(1, 0);
            }
        }
        ctx.set_totals_final();
    }
    if (ctx.cancelled()) {
        finish_remaining_cancelled(ctx, sources, 0);
        return ok();
    }
    if (space > 0) {
        if (const auto avail = available_bytes(dest); avail && *avail < space) {
            return fail(ErrorCode::NoSpace, "not enough space in " + dest.native());
        }
    }
    PhaseScope working(ctx, OpPhase::Working);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!ctx.checkpoint()) {
            finish_remaining_cancelled(ctx, sources, i);
            break;
        }
        const Path& src = sources[i];
        if (pre[i]) {
            ctx.finish_item(failed_item(src, pre[i].value_or(Error{})));
            continue;
        }
        ctx.add_touched(src.parent_path());
        if (r.options.dry_run) {
            ItemOutcome o;
            o.source = src;
            o.destination = dest / src.filename();
            struct stat st {};
            if (same_dir(src.parent_path(), dest)) {
                o.status = ItemStatus::Skipped;
            } else if (lstat_path(o.destination, st) == 0) {
                o.status = ItemStatus::Conflict;
            }
            ctx.finish_item(std::move(o));
            continue;
        }
        ctx.set_current(src);
        if (ctx.inject(src)) {
            ctx.finish_item(
                failed_item(src, make_error(ErrorCode::IoError, "injected: " + src.native())));
            continue;
        }
        MoveParams mp;
        mp.step_kind = OpKind::Move;
        MoveResult mr = move_item(ctx, src, dest, src.filename().native(), mp, trees[i].get());
        ctx.add_steps(std::move(mr.steps));
        const bool stop = mr.outcome.status == ItemStatus::Cancelled;
        ctx.finish_item(std::move(mr.outcome));
        if (stop) {
            finish_remaining_cancelled(ctx, sources, i + 1);
            break;
        }
    }
    return ok();
}

// ---------------------------------------------------------------------------------------------
// Rename
// ---------------------------------------------------------------------------------------------

Result<void> run_rename(Context& ctx) {
    const OpRequest& r = ctx.request();
    const Path& src = r.sources.front();
    ctx.set_top_level_total(1);
    ctx.add_total(1, 0);
    ctx.set_totals_final();
    ctx.add_touched(src.parent_path());
    PhaseScope working(ctx, OpPhase::Working);
    if (auto e = check_source(ctx, src, true, nullptr)) {
        ctx.finish_item(failed_item(src, *e));
        return ok();
    }
    if (src.filename().native() == r.new_name) {
        ItemOutcome o;
        o.source = src;
        o.destination = src;
        o.status = ItemStatus::Skipped;
        ctx.finish_item(std::move(o));
        return ok();
    }
    if (r.options.dry_run) {
        ItemOutcome o;
        o.source = src;
        o.destination = src.parent_path() / r.new_name;
        struct stat st {};
        if (lstat_path(o.destination, st) == 0) {
            o.status = ItemStatus::Conflict;
        }
        ctx.finish_item(std::move(o));
        return ok();
    }
    ctx.set_current(src);
    if (ctx.inject(src)) {
        ctx.finish_item(
            failed_item(src, make_error(ErrorCode::IoError, "injected: " + src.native())));
        return ok();
    }
    MoveParams mp;
    mp.step_kind = OpKind::Rename;
    mp.allow_cross_device = false;
    MoveResult mr = move_item(ctx, src, src.parent_path(), r.new_name, mp, nullptr);
    ctx.add_steps(std::move(mr.steps));
    ctx.finish_item(std::move(mr.outcome));
    return ok();
}

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------

Result<OpResult> run_operation_ex(const OpRequest& request, const EngineOptions& options,
                                  const ConflictResolver& resolve, const ProgressSink& progress,
                                  std::stop_token stop, const RunHooks* hooks) {
    try {
        if (auto v = validate_request(request, options.protected_paths, options.trash.get()); !v) {
            return Unexpected<Error>(v.error());
        }
        Context ctx(request, options, resolve, progress, std::move(stop), hooks);
        ctx.emit(true);
        Result<void> run = ok();
        OpKind record_kind = request.kind;
        Path record_dest = request.destination;
        switch (request.kind) {
        case OpKind::Copy: run = run_copy(ctx); break;
        case OpKind::Move: run = run_move(ctx); break;
        case OpKind::Rename:
            run = run_rename(ctx);
            record_dest = request.sources.front().parent_path();
            break;
        case OpKind::MakeDirectory: run = run_make_directory(ctx); break;
        case OpKind::CreateSymlink: run = run_create_symlink(ctx); break;
        case OpKind::Trash:
            run = run_trash(ctx);
            record_dest.clear();
            break;
        case OpKind::Restore:
            run = run_restore(ctx);
            record_dest.clear();
            break;
        case OpKind::DeletePermanently: run = run_delete(ctx); break;
        case OpKind::Undo:
            run = run_undo(ctx);
            record_kind = request.record->kind;
            record_dest = request.record->destination;
            break;
        case OpKind::Redo:
            run = run_redo(ctx);
            record_kind = request.record->kind;
            record_dest = request.record->destination;
            break;
        }
        if (!run) {
            return Unexpected<Error>(run.error());
        }
        return ctx.finish(record_kind, record_dest);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory");
    } catch (...) {
        return fail(ErrorCode::Unknown, "unexpected exception in the ops engine");
    }
}

} // namespace navigator::ops::detail

namespace navigator::ops {

Result<OpResult> run_operation(const OpRequest& request, const EngineOptions& options,
                               const ConflictResolver& resolve, const ProgressSink& progress,
                               std::stop_token stop) {
    return detail::run_operation_ex(request, options, resolve, progress, std::move(stop), nullptr);
}

} // namespace navigator::ops
