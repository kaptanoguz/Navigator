// src/ops/conflict.cpp: see conflict.hpp.
#include "conflict.hpp"

#include <cerrno>
#include <string>

#include "sys.hpp"

namespace navigator::ops::detail {
namespace {

constexpr int kMaxAsks = 4;
constexpr unsigned kMaxKeepBoth = 100000;

[[nodiscard]] Placement make(Placement::Action action, std::string name,
                             const struct stat& existing) {
    Placement p;
    p.action = action;
    p.name = std::move(name);
    p.existing = existing;
    return p;
}

[[nodiscard]] Placement keep_both(const ConflictInput& in, bool is_dir) {
    std::string name;
    const int rc = first_free_name(in.dst_dirfd, in.name, is_dir, name);
    if (rc != 0) {
        Placement p = make(Placement::Action::Fail, in.name, in.existing);
        p.error = errno_error(rc, "no free name for", in.dst_dir / in.name);
        return p;
    }
    return make(Placement::Action::KeepBoth, std::move(name), in.existing);
}

} // namespace

int first_free_name(int dirfd, std::string_view name, bool is_dir, std::string& out) {
    for (unsigned n = 2; n < kMaxKeepBoth; ++n) {
        std::string candidate = keep_both_name(name, n, is_dir);
        struct stat st {};
        const int rc = lstat_at(dirfd, candidate, st);
        if (rc == ENOENT) {
            out = std::move(candidate);
            return 0;
        }
        if (rc != 0) {
            return rc;
        }
    }
    return EEXIST;
}

bool replace_can_trash(const Context& ctx, const Path& path) {
    if (ctx.trash() == nullptr || !ctx.opt().trash_replaced) {
        return false;
    }
    return ctx.trash()->trash_dir_for(path).has_value();
}

Result<trash::TrashedItem> trash_existing(Context& ctx, int dirfd, const Path& dir,
                                          const std::string& name, const struct stat& existing) {
    if (ctx.trash() == nullptr) {
        return fail(ErrorCode::Unsupported, "no trash available");
    }
    struct stat now {};
    const int rc = lstat_at(dirfd, name, now);
    if (rc != 0) {
        return Unexpected<Error>(errno_error(rc, "cannot stat", dir / name));
    }
    if (!same_inode(now, existing)) {
        return fail(ErrorCode::AlreadyExists,
                    "the existing item changed: " + (dir / name).native());
    }
    return ctx.trash()->trash(dir / name);
}

Placement resolve_placement(Context& ctx, const ConflictInput& in) {
    const Path existing_path = in.dst_dir / in.name;
    const bool src_dir = in.source == nullptr || S_ISDIR(in.source->st_mode);
    const bool dst_dir = S_ISDIR(in.existing.st_mode);
    const bool same_file = in.source != nullptr && same_inode(*in.source, in.existing);

    if (in.force_keep_both) {
        return keep_both(in, src_dir);
    }

    // What Replace and Merge would mean here (S8).
    bool replace_allowed =
        in.replace_supported && !same_file &&
        (in.source == nullptr || !is_same_or_inside(in.source_path, existing_path));
    bool permanent = false;
    if (replace_allowed) {
        if (!replace_can_trash(ctx, existing_path)) {
            // No trash copy: only a non-directory may be overwritten (by rename), never a folder,
            // never by a folder, never during undo / redo.
            if (dst_dir || src_dir || !in.permanent_replace_ok) {
                replace_allowed = false;
            } else {
                permanent = true;
            }
        }
    }
    const bool merge_allowed =
        in.merge_supported && src_dir && dst_dir && !same_file && in.source != nullptr;

    auto replace = [&]() {
        Placement p = make(Placement::Action::Replace, in.name, in.existing);
        p.permanent = permanent;
        return p;
    };
    auto refuse = [&](const char* why) {
        Placement p = make(Placement::Action::Fail, in.name, in.existing);
        p.error =
            make_error(ErrorCode::Unsupported, std::string(why) + ": " + existing_path.native());
        return p;
    };

    switch (ctx.request().conflict) {
    case ConflictPolicy::Skip: return make(Placement::Action::Skip, in.name, in.existing);
    case ConflictPolicy::KeepBoth: return keep_both(in, src_dir);
    case ConflictPolicy::Replace:
        return replace_allowed ? replace() : refuse("cannot replace this item safely");
    case ConflictPolicy::ReplaceIfNewer:
        if (src_dir && dst_dir) {
            return merge_allowed ? make(Placement::Action::Merge, in.name, in.existing)
                                 : make(Placement::Action::Skip, in.name, in.existing);
        }
        if (!src_dir && !dst_dir && in.source != nullptr &&
            to_timestamp(in.source->st_mtim) > to_timestamp(in.existing.st_mtim)) {
            return replace_allowed ? replace() : refuse("cannot replace this item safely");
        }
        return make(Placement::Action::Skip, in.name, in.existing);
    case ConflictPolicy::Ask: break;
    }

    Conflict c;
    if (in.source != nullptr) {
        c.source = side_from(in.source_path, *in.source);
    } else {
        c.source.path = existing_path;
        c.source.kind = fs::EntryKind::Directory;
    }
    c.existing = side_from(existing_path, in.existing);
    {
        std::string suggestion;
        if (first_free_name(in.dst_dirfd, in.name, src_dir, suggestion) == 0) {
            c.keep_both_name = std::move(suggestion);
        }
    }
    c.replace_allowed = replace_allowed;
    c.replace_is_permanent = replace_allowed && permanent;
    c.merge_allowed = merge_allowed;
    c.same_file = same_file;
    c.source_is_newer = in.source != nullptr && c.source.mtime > c.existing.mtime;
    const Shape shape = src_dir && dst_dir     ? Shape::DirOntoDir
                        : !src_dir && !dst_dir ? Shape::FileOntoFile
                                               : Shape::Mixed;

    for (int attempt = 0; attempt < kMaxAsks; ++attempt) {
        const ConflictDecision d = ctx.ask(c, shape);
        switch (d.action) {
        case ConflictAction::Skip: return make(Placement::Action::Skip, in.name, in.existing);
        case ConflictAction::Cancel:
            ctx.cancel();
            return make(Placement::Action::Cancel, in.name, in.existing);
        case ConflictAction::Replace:
            if (replace_allowed) {
                return replace();
            }
            continue; // not offered: ask again
        case ConflictAction::Merge:
            if (merge_allowed) {
                return make(Placement::Action::Merge, in.name, in.existing);
            }
            continue;
        case ConflictAction::KeepBoth: {
            if (d.new_name.empty()) {
                return keep_both(in, src_dir);
            }
            if (validate_new_name(d.new_name) != NameError::None) {
                continue;
            }
            struct stat st {};
            if (lstat_at(in.dst_dirfd, d.new_name, st) == ENOENT) {
                return make(Placement::Action::KeepBoth, d.new_name, in.existing);
            }
            // Taken too: ask again about the same conflict with a fresh suggestion.
            std::string suggestion;
            if (first_free_name(in.dst_dirfd, in.name, src_dir, suggestion) == 0) {
                c.keep_both_name = std::move(suggestion);
            }
            continue;
        }
        }
    }
    return make(Placement::Action::Skip, in.name, in.existing);
}

} // namespace navigator::ops::detail
