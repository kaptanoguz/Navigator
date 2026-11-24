// navigator/ops.hpp
//
// Purpose: file operations (Phase 7): copy, move, rename, new folder, symlink, trash, restore
//          and the separately named permanent delete; their conflicts, progress and results;
//          the background operation queue; the undo/redo stack; pure name and path helpers.
//          This is the first contract that MUTATES the user's files. The safety rules S1..S16
//          below are normative for every implementation and are repeated in
//          docs/architecture.md "Phase 7".
// Owner:   contracts (header); implementation in src/ops (role: ops, new library
//          `navigator_ops`), tests in tests/ops (temporary fixture directories only).
// Thread-safety:
//   - Pure helpers (to_string, keep_both_name, split_extension, stem_length,
//     validate_new_name, is_same_or_inside, protection_of, make_label) and all value types: any
//     thread, including the UI thread.
//   - run_operation(): blocking; call on a worker thread (tests call it directly).
//   - IOperationQueue: every member is thread-safe and NON-blocking (it only takes an internal
//     mutex); the ui calls submit / resolve / cancel / pause / poll / snapshot on the UI thread.
//     Work runs on the queue's own worker threads. `on_event` is called on a worker.
//   - UndoStack: NOT thread-safe; owned and used by the UI thread only.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions. Per-item
//          failures are values in OpResult (an operation with failed items still "succeeds" as
//          a call); a Result error means the request itself was rejected or the engine failed.
//
// Safety rules (normative):
//   S1  No silent removal. Nothing in src/ops unlinks or rmdirs user data except (a) the
//       DeletePermanently kind with OpRequest::confirmed == true, (b) the source of a
//       cross-filesystem Move after S6 verification, (c) rmdir of an EMPTY directory that an
//       undo record says Navigator created, (d) the engine's own temp files and the partial
//       destination of the item being copied when a cross-filesystem Move is cancelled or
//       fails (S9). Undo removes created items by moving them to the trash (S12).
//   S2  Delete means trash. file.trash submits OpKind::Trash. When an item's filesystem has no
//       usable trash, its outcome is ItemStatus::Failed with ErrorCode::Unsupported and the
//       item is listed in OpResult::untrashable; the engine NEVER falls back to permanent
//       deletion. The ui may then offer the confirmed permanent-delete sheet for those items.
//   S3  Permanent delete is a separate kind (DeletePermanently) that submit() rejects unless
//       OpRequest::confirmed is true. Only the ui's confirmation sheet sets it. It is never
//       produced by undo, redo, paste, drag and drop or a conflict decision.
//   S4  Symlinks are never followed when deleting, trashing, moving or walking a tree: links
//       are copied, moved, trashed and deleted as links (readlink + symlink for copies).
//       OpOptions::dereference_top_level may copy the TARGET of a top-level link only (Copy).
//   S5  One filesystem per tree. Recursive walks (copy scan, permanent delete, cross-fs move
//       source removal) do not descend into a directory whose st_dev differs from its
//       top-level item's: a mount point inside a tree is reported (ItemStatus::Failed,
//       ErrorCode::Unsupported, "mount point inside") and left alone. Walks use dirfd-relative
//       calls (openat with O_NOFOLLOW | O_DIRECTORY, fstatat AT_SYMLINK_NOFOLLOW, unlinkat) so
//       a path swapped for a symlink mid-walk is never followed.
//   S6  Copy writes into a temp name in the destination directory
//       (".<name>.navigator-<random>.part", O_CREAT | O_EXCL, mode 0600), applies metadata
//       (S10), then renames it to the final name with renameat2(RENAME_NOREPLACE). A failure
//       or cancellation unlinks the temp file: a destination name never holds a truncated
//       file. Where RENAME_NOREPLACE is unsupported (EINVAL: some FUSE/NFS), the engine uses
//       link(2) + unlink(temp) (atomic EEXIST), and only where hard links are unsupported too
//       (vfat, exfat) a check-then-rename with the race documented in the architecture doc.
//       Directories are created with mkdir(2) under their final name (atomic EEXIST) and
//       filled; their mtime is applied after their contents (post-order).
//   S7  Move within one filesystem is renameat2(RENAME_NOREPLACE) (plain renameat only after an
//       explicit Replace decision, S8). Across filesystems (EXDEV) it is copy (S6), then
//       verification, then removal of the source: every destination file is fsync'ed (and its
//       directory) and each source file must be unchanged during its copy (fstat before and
//       after: size, mtime, FileId); only entries recorded in the scan are removed, bottom up,
//       without following links (S4, S5). A source entry that changed or appeared during the
//       move is left in place and reported; its directory then stays (not empty).
//   S8  Overwrite only after an explicit decision (ConflictPolicy::Replace, ReplaceIfNewer, or a
//       ConflictDecision with Replace). Replace first moves the existing item to the trash
//       (OpOptions::trash_replaced, default true; recorded for undo), then renames the new item
//       into place; if the trash is unavailable the existing FILE is overwritten atomically by
//       rename (Conflict::replace_is_permanent warns the ui before the decision) and the undo
//       record is marked lossy. An existing DIRECTORY is never overwritten permanently: when it
//       cannot be trashed, Replace is not offered (Merge, Skip, KeepBoth remain).
//   S9  Cancellation stops at the next check (at least every copy_chunk_bytes and between
//       entries), removes the current temp file, and for a cross-filesystem Move also removes
//       the partial destination of the CURRENT top-level item (its source is intact). Items
//       already completed stay done and are in OpResult::record for undo. Nothing is rolled
//       back automatically.
//   S10 Metadata: permission bits (rwx for u/g/o and the sticky bit) and mtime (atime too) are
//       preserved by default; setuid/setgid are dropped unless the new file keeps the source's
//       owner and group. Ownership is never changed (no chown) unless the process is root and
//       OpOptions::preserve_owner. Extended attributes: names in "user." and the POSIX ACLs
//       "system.posix_acl_access" / "system.posix_acl_default" are copied when
//       preserve_xattrs; "security.*" and "trusted.*" never (the destination's policy labels
//       new files). xattr failures (ENOTSUP on vfat, ...) never fail an item; they are counted
//       in OpResult::metadata_warnings.
//   S11 Special files: FIFOs are recreated with mkfifo (never opened: reading a FIFO blocks);
//       sockets and block/char devices are not copied (Failed, Unsupported) but can be moved
//       or renamed within a filesystem and trashed. Hard links: files of one copied tree that
//       share a FileId are linked again at the destination when it is one filesystem
//       (preserve_hardlinks), else copied separately. Sparse files keep their holes
//       (SEEK_DATA / SEEK_HOLE) where the destination supports them; reflink (FICLONE) and
//       copy_file_range are tried first.
//   S12 Every completed top-level step is recorded (UndoStep) with the identity of what it
//       produced. Undo never deletes data: created copies and created links go to the trash;
//       a folder Navigator made is rmdir'ed only while it is still empty (S1c), else it is
//       left alone (Changed). Undo verifies identity first (FileId, size, mtime, kind: ItemGuard)
//       and skips an item that changed since (ItemStatus::Changed). Undo never overwrites: an item
//       that appeared at the path it would restore becomes a Conflict (policy Ask).
//   S13 Refused requests (submit() fails, nothing runs): relative or non-normal source paths;
//       a Move / Copy of a directory into itself or below itself (is_same_or_inside); invalid
//       new names (validate_new_name); and for Move, Rename, Trash and DeletePermanently any
//       source that protection_of() reports (/, the home directory or an ancestor of it, a
//       mount point, a trash directory: use the Trash view). Copying such paths is allowed.
//   S14 Space: before writing, a Copy (and a cross-filesystem Move) checks the destination
//       volume's available bytes (statvfs f_bavail) against the scanned total and fails the
//       operation with NoSpace before writing anything when it cannot fit.
//   S15 Paths in one operation: the engine re-checks every condition at execution time (the
//       ui's view may be stale); submit-time checks are lexical only. `trashed` items of
//       Restore / DeletePermanently are the one exception to the Trash protection: they are
//       addressed by TrashedItem, never by a path inside a trash directory.
//   S16 Tests that create, move or delete files use temporary fixture directories only
//       (CLAUDE.md). tests/ops never touches $HOME or the real trash: ITrash is created with
//       TrashOptions::home_trash inside the fixture, or faked.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/trash.hpp>
#include <navigator/types.hpp>

namespace navigator::ops {

// ---------------------------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------------------------

// Runtime identity of a submitted operation; unique within one queue, never 0, never reused.
// Command::arg carries `value` for ops.cancel / ops.toggle_pause (commands.hpp).
struct OpId {
    std::uint64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(OpId, OpId) = default;
    friend constexpr auto operator<=>(OpId, OpId) = default;
};

enum class OpKind : std::uint8_t {
    Copy = 0,              // sources -> into `destination` (a folder)
    Move = 1,              // sources -> into `destination` (S7)
    Rename = 2,            // sources[0] -> same folder, `new_name`. When the existing target
                           // is the same FileId (case-only rename on a case-insensitive
                           // filesystem) it is not a conflict: renamed through a temp name
    MakeDirectory = 3,     // `destination` / `new_name` (mkdir 0777 & ~umask)
    CreateSymlink = 4,     // one link per source in `destination`, named like the source,
                           // pointing at the source's absolute path (drop "Link here")
    Trash = 5,             // sources -> their filesystem's trash (S2, trash.hpp)
    Restore = 6,           // `trashed` items -> their original paths (or KeepBoth names)
    DeletePermanently = 7, // sources and/or `trashed` items, removed for good (S1a, S3)
    Undo = 8,              // run the inverse of `record` (S12)
    Redo = 9,              // run `record` again
    BatchRename = 10,      // Phase 9B: sources[i] -> same folder, new_names[i] (staged for swaps)
};

// Stable lowercase identifier for logs and tests ("copy", "move", "rename", "make_directory",
// "create_symlink", "trash", "restore", "delete_permanently", "undo", "redo", "batch_rename").
// Pure.
[[nodiscard]] constexpr std::string_view to_string(OpKind kind) noexcept {
    switch (kind) {
    case OpKind::Copy: return "copy";
    case OpKind::Move: return "move";
    case OpKind::Rename: return "rename";
    case OpKind::MakeDirectory: return "make_directory";
    case OpKind::CreateSymlink: return "create_symlink";
    case OpKind::Trash: return "trash";
    case OpKind::Restore: return "restore";
    case OpKind::DeletePermanently: return "delete_permanently";
    case OpKind::Undo: return "undo";
    case OpKind::Redo: return "redo";
    case OpKind::BatchRename: return "batch_rename";
    }
    return "copy";
}

// What to do when a destination name is taken.
enum class ConflictPolicy : std::uint8_t {
    Ask = 0,            // raise a Conflict and wait for a ConflictDecision (default)
    Skip = 1,           // leave both; the item is Skipped
    Replace = 2,        // S8
    KeepBoth = 3,       // place the new item under keep_both_name(name, n), n = 2, 3, ...
    ReplaceIfNewer = 4, // Replace when the source mtime is newer, else Skip (files only;
                        // directories conflicting with directories merge)
};

// Answer to one Conflict.
enum class ConflictAction : std::uint8_t {
    Skip = 0,
    Replace = 1,  // not allowed when Conflict::replace_allowed is false (InvalidArgument)
    KeepBoth = 2, // Conflict::keep_both_name, or ConflictDecision::new_name when non-empty
    Merge = 3,    // directory onto directory only: recurse; contained names may conflict again
    Cancel = 4,   // cancel the whole operation (as IOperationQueue::cancel)
};

struct ConflictDecision {
    ConflictAction action = ConflictAction::KeepBoth; // the sheet's safe default (Enter)
    // Apply the same action to every later conflict of the same shape in this operation
    // (file-onto-file, or directory-onto-directory) without asking again.
    bool apply_to_all = false;
    // KeepBoth only: a user-typed name instead of the suggested one (validate_new_name rules;
    // if it is taken too, the engine asks again).
    std::string new_name;
};

// Identity of a file as last seen; used by undo/redo verification (S12) and by conflicts.
struct ItemGuard {
    Path path;
    fs::EntryKind kind = fs::EntryKind::Unknown; // lstat kind (Symlink for links)
    FileId id;
    std::uint64_t size = 0; // files and links; 0 for directories
    Timestamp mtime{};

    friend bool operator==(const ItemGuard&, const ItemGuard&) = default;
};

// One side of a conflict, for the conflict sheet (both shown side by side with the Phase 6
// thumbnail, size and modified date).
struct ConflictSide {
    Path path;
    fs::EntryKind kind = fs::EntryKind::Unknown;
    std::uint64_t size = 0;
    Timestamp mtime{};
    std::uint32_t mode = 0;
};

// Runtime identity of a pending conflict; unique within one queue, never 0.
struct ConflictId {
    std::uint64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(ConflictId, ConflictId) = default;
};

struct Conflict {
    ConflictId id;
    OpId op;
    OpKind kind = OpKind::Copy;        // the operation's kind (Undo/Redo show what they undo)
    ConflictSide source;               // the item being placed
    ConflictSide existing;             // what occupies the destination name
    std::string keep_both_name;        // first free keep_both_name() candidate at raise time
    bool replace_allowed = true;       // false: existing directory that cannot be trashed (S8)
    bool replace_is_permanent = false; // Replace would overwrite without a trash copy (S8)
    bool merge_allowed = false;        // both are directories
    bool same_file = false;            // source and existing are the same FileId (Skip / KeepBoth)
    bool source_is_newer = false;      // source.mtime > existing.mtime
    std::size_t remaining_items = 0;   // top-level items still to do (for the "apply to all" label)
};

// Options of one request. Defaults are the safe ones.
struct OpOptions {
    bool preserve_mode = true;          // S10
    bool preserve_times = true;         // S10 (mtime and atime)
    bool preserve_xattrs = true;        // S10 (user.* and POSIX ACLs; best effort)
    bool preserve_owner = false;        // S10 (only effective as root)
    bool preserve_hardlinks = true;     // S11
    bool trash_replaced = true;         // S8
    bool dereference_top_level = false; // S4: Copy the target of a top-level symlink
    bool sync_copies = false;           // fsync every copied file (always on for cross-fs Move, S7)
    // Scan and plan only: totals, protections, conflicts (reported as ItemStatus::Conflict
    // outcomes, never asked) and free space; nothing is created, moved or removed.
    bool dry_run = false;
};

// Who asked for an operation; decides how its UndoRecord enters the UndoStack.
enum class OpOrigin : std::uint8_t {
    User = 0, // commands, paste, drag and drop: new undo entry, redo history cleared
    Undo = 1, // produced by UndoStack::take_undo: its record goes to the redo side
    Redo = 2, // produced by UndoStack::take_redo: its record goes back to the undo side
};

struct UndoRecord; // below

struct OpRequest {
    OpKind kind = OpKind::Copy;
    OpOrigin origin = OpOrigin::User;
    // Copy, Move, CreateSymlink, Trash, DeletePermanently: the items (absolute, lexically
    // normal, top-level; duplicates are dropped). Rename: exactly one.
    std::vector<Path> sources;
    // Copy, Move, CreateSymlink: target folder. MakeDirectory: parent folder. Others: unused.
    Path destination;
    // Rename: the new name in the same folder. MakeDirectory: the folder name. Others: unused.
    std::string new_name;
    // BatchRename (Phase 9B): parallel to `sources`; sources[i] is renamed to new_names[i] in its
    // own parent folder. Two-phase staging handles swaps/cycles atomically.
    std::vector<std::string> new_names;
    // Restore and DeletePermanently (Trash view, trash.empty): items from ITrash::list().
    std::vector<trash::TrashedItem> trashed;
    // Undo / Redo: the record to invert / repeat (from UndoStack).
    std::shared_ptr<const UndoRecord> record;
    // Default policy for name conflicts. A Copy into the sources' own folder (paste into the
    // same folder, Ctrl+drag onto it) always uses KeepBoth for those items, like GNOME Files,
    // whatever this says; a Move of an item into its own folder is Skipped (nothing to do).
    ConflictPolicy conflict = ConflictPolicy::Ask;
    OpOptions options;
    // S3: must be true for DeletePermanently, set only by the ui's confirmation sheet.
    bool confirmed = false;
};

// ---------------------------------------------------------------------------------------------
// Progress, results, undo records
// ---------------------------------------------------------------------------------------------

enum class OpPhase : std::uint8_t {
    Queued = 0,   // waiting for a worker or for an earlier operation on the same paths
    Scanning = 1, // counting items and bytes (totals not final)
    Working = 2,
    Waiting = 3, // a Conflict is pending
    Paused = 4,
    Finishing = 5, // fsync, removing cross-fs move sources, applying directory times
    Done = 6,
};

struct OpProgress {
    OpId id;
    OpKind kind = OpKind::Copy;
    OpOrigin origin = OpOrigin::User;
    OpPhase phase = OpPhase::Queued;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0; // grows while Scanning; final once totals_final
    std::uint64_t items_done = 0;  // entries of all trees (files, folders, links)
    std::uint64_t items_total = 0;
    bool totals_final = false;
    std::size_t top_level_done = 0; // of request.sources / trashed
    std::size_t top_level_total = 0;
    Path current;     // entry being processed (the ui middle-elides it)
    Path destination; // the request's destination folder (empty when none)
    // Bytes per second over a sliding window; 0 until at least 2 s of stable samples
    // (docs/review/phase-6.md). eta is empty while the rate is 0 or the totals are not final.
    double bytes_per_second = 0.0;
    std::optional<std::chrono::seconds> eta;
    std::chrono::milliseconds elapsed{0}; // excluding paused and waiting time
    std::size_t failed_so_far = 0;
};

enum class ItemStatus : std::uint8_t {
    Done = 0,
    Skipped = 1,   // conflict Skip, ReplaceIfNewer not newer, or a Move onto itself
    Failed = 2,    // `error` says why (NoSpace, PermissionDenied, Unsupported, ...)
    Cancelled = 3, // not reached, or interrupted (S9)
    Changed = 4,   // undo/redo guard mismatch (S12): left untouched
    Conflict = 5,  // dry_run only: the name is taken
};

// Outcome of one top-level item.
struct ItemOutcome {
    Path source;      // as requested (Restore / trashed deletes: the trashed item's files path)
    Path destination; // where it ended up (empty when it did not)
    ItemStatus status = ItemStatus::Done;
    std::optional<Error> error;
    bool partial = false; // a directory whose tree completed only in part (some entries failed)
};

// One undoable top-level step (inverse table below, at make_label).
struct UndoStep {
    OpKind kind = OpKind::Copy; // the forward kind: Copy, Move, Rename, MakeDirectory,
                                // CreateSymlink, Trash or Restore
    Path before; // the path it had (Copy: the copy's source; MakeDirectory/CreateSymlink: empty)
    ItemGuard after; // what the step produced, captured when it completed (S12)
    // Trash: where the item went. Restore: the trash item it came from (for redo).
    std::optional<trash::TrashedItem> trashed;
    // S8: an existing item that this step replaced and moved to the trash (undo restores it).
    std::optional<trash::TrashedItem> replaced;
    bool replaced_permanently = false; // S8: the replaced file is gone; undo cannot bring it back
    // Parent folders this step created (Restore into a missing folder), outermost first; undo
    // rmdirs them when empty (S1c).
    std::vector<ItemGuard> created_parents;
};

// Runtime identity of an undo record; unique within one UndoStack, never 0.
struct UndoId {
    std::uint64_t value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(UndoId, UndoId) = default;
};

// One user-visible undo entry = one finished operation (possibly cancelled part way: only the
// completed steps are in it). DeletePermanently never produces a record (not undoable; the ui
// says so before confirming and in the notice afterwards).
struct UndoRecord {
    UndoId id;                   // assigned by UndoStack::record_done
    OpKind kind = OpKind::Copy;  // the forward operation (never Undo / Redo / DeletePermanently)
    std::vector<UndoStep> steps; // in execution order; undo runs them in reverse
    Path destination;            // the forward request's destination folder (label, Inspector)
    bool lossy = false;          // some step has replaced_permanently
    // Mount points of the volumes the steps touched. Before submitting an undo the ui checks
    // that they are still mounted and otherwise says which volume is missing instead of
    // failing item by item (docs/review/phase-6.md).
    std::vector<Path> volumes;
    std::chrono::system_clock::time_point finished_at{};
};

struct OpResult {
    OpId id;
    OpKind kind = OpKind::Copy;
    OpOrigin origin = OpOrigin::User;
    bool cancelled = false;
    std::vector<ItemOutcome> items; // one per top-level item, request order
    std::size_t done = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    std::size_t changed = 0;
    std::uint64_t bytes_done = 0;
    std::size_t metadata_warnings = 0; // S10 (xattrs/ACL/times not applied)
    // S2: Trash items whose filesystem has no usable trash (the ui may offer a confirmed
    // permanent delete for exactly these).
    std::vector<Path> untrashable;
    // Folders whose contents changed (the ui refreshes those not covered by a watch, e.g. on
    // network filesystems; watched folders reconcile through watch events).
    std::vector<Path> touched_folders;
    // Completed steps for the UndoStack; nullopt when nothing undoable completed (or dry_run,
    // or DeletePermanently).
    std::optional<UndoRecord> record;
};

// Inverse of each forward step (S12), executed by OpKind::Undo in reverse step order:
//   Copy          -> the copy (after) goes to the trash; then a `replaced` item is restored
//   Move          -> moved back from after.path to `before` (S7 rules; conflict if taken)
//   Rename        -> renamed back to `before` (conflict if taken)
//   MakeDirectory -> rmdir(after.path) only while it is empty and matches `after`; else Changed
//   CreateSymlink -> the link goes to the trash
//   Trash         -> `trashed` restored to `before` (conflict if taken)
//   Restore       -> after.path goes to the trash again
// Then `created_parents` are rmdir'ed innermost first while empty. Redo repeats the forward
// step with the same guards on the inputs (Redo of Copy copies `before` again).

// Text for "Undo: move 3 items to Photos" (docs/review/phase-6.md). The ui translates
// `key` with the arguments; Navigator ships English and Turkish strings for:
//   "undo.label.copy" / ".move" / ".rename" / ".make_directory" / ".create_symlink" /
//   ".trash" / ".restore", each with {count}, {name} (first item's display name) and
//   {folder} (display name of the destination folder; empty when not applicable).
struct UndoLabel {
    std::string_view key;
    std::size_t count = 0;
    std::string name;   // fs::to_display_name of the first step's item
    std::string folder; // fs::to_display_name of the record's destination folder
};
[[nodiscard]] UndoLabel make_label(const UndoRecord& record);

// ---------------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------------

// Splits a file name into stem and extension (with its dot) for KeepBoth names and for the
// rename editor's initial selection. Rules:
//   - a directory has no extension: {"my.folder", ""};
//   - a dot-file without a further dot has none: ".bashrc" -> {".bashrc", ""};
//     ".config.json" -> {".config", ".json"};
//   - the last ".ext" is the extension, except that ".tar" before a compression extension
//     (gz, bz2, xz, zst, lz, lzma, lz4, Z, br; ASCII case-insensitive) is kept with it:
//     "a.tar.gz" -> {"a", ".tar.gz"}, "a.b.txt" -> {"a.b", ".txt"};
//   - a trailing dot or an extension longer than 16 bytes is not an extension:
//     "notes." -> {"notes.", ""}.
struct NameParts {
    std::string_view stem;
    std::string_view extension; // empty or starts with '.'
};
[[nodiscard]] NameParts split_extension(std::string_view name, bool is_directory) noexcept;

// Byte length of split_extension(name, is_directory).stem: the rename editor selects
// [0, stem_length) initially, so typing replaces the name and keeps the extension.
[[nodiscard]] std::size_t stem_length(std::string_view name, bool is_directory) noexcept;

// "<stem> (n)<extension>" with split_extension rules; n >= 2 (n < 2 is treated as 2). A stem
// that already ends with " (k)" (k a decimal number) has that suffix replaced, so repeated
// copies give "a (2).txt", "a (3).txt", not "a (2) (2).txt". The pattern is not localized
// (stable, language-independent names). When the result would exceed kNameMaxBytes the stem is
// shortened at a UTF-8 code point boundary. Pure.
inline constexpr std::size_t kNameMaxBytes = 255; // NAME_MAX on Linux filesystems
[[nodiscard]] std::string keep_both_name(std::string_view name, unsigned n, bool is_directory);

enum class NameError : std::uint8_t {
    None = 0,
    Empty = 1,
    Dot = 2,    // "."
    DotDot = 3, // ".."
    ContainsSlash = 4,
    ContainsNul = 5,
    TooLong = 6, // more than kNameMaxBytes bytes (Turkish "ğüşıöç" count 2 bytes each)
};

// Checks a name typed in the rename editor, the new-folder editor or a KeepBoth field. Any
// other byte sequence is accepted (spaces, leading dots, non-UTF-8 bytes from a paste: Linux
// allows them; the destination filesystem may still refuse, e.g. ':' on vfat, which the engine
// reports as InvalidArgument). The ui warns, without refusing, about leading/trailing spaces
// and a new leading '.' (the item becomes hidden). Pure.
[[nodiscard]] NameError validate_new_name(std::string_view name) noexcept;

// True when `path` equals `ancestor` or lies below it, compared component-wise after lexical
// normalization ("/a/b" is inside "/a", "/ab" is not inside "/a"). Lexical only (S15). Pure.
[[nodiscard]] bool is_same_or_inside(const Path& path, const Path& ancestor);

enum class Protection : std::uint8_t {
    None = 0,
    Root = 1,           // "/"
    Home = 2,           // the home directory itself
    AncestorOfHome = 3, // e.g. "/home"
    MountPoint = 4,     // a volume's mount point
    Trash = 5,          // a trash directory or inside one (trash::ITrash::is_inside_trash)
    Relative = 6,       // not absolute: never acted on
};

// Paths that Move, Rename, Trash and DeletePermanently refuse as SOURCES (S13). The ui fills
// it from platform::user_dir(Home), its volume list and the trash's directories, and passes it
// to the queue (set_protected_paths) whenever the volume list changes.
struct ProtectedPaths {
    Path home;
    std::vector<Path> mount_points;
    std::vector<Path> trash_roots; // TrashDir::root of every trash, including the home trash
};

// Lexical classification of `path` against `protected_paths`, first match in enum order. Pure.
[[nodiscard]] Protection protection_of(const Path& path, const ProtectedPaths& protected_paths);

// ---------------------------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------------------------

// Synchronous conflict answer for run_operation() (tests, worker internals).
using ConflictResolver = std::function<ConflictDecision(const Conflict&)>;
// Progress sink for run_operation(); called at most every progress_interval.
using ProgressSink = std::function<void(const OpProgress&)>;

struct EngineOptions {
    // Required for Trash, Restore, Undo of Copy/MakeDirectory/CreateSymlink/Trash, and for
    // Replace with trash_replaced. nullptr: those steps fail with Unsupported (never a
    // permanent fallback, S2).
    std::shared_ptr<trash::ITrash> trash;
    ProtectedPaths protected_paths;
    std::size_t copy_chunk_bytes = std::size_t{1} << 20U; // read/write unit; cancel granularity
    std::chrono::milliseconds progress_interval{100};
    // Testing seam: called before each entry is processed with its path; returning false makes
    // that entry fail with IoError ("injected"). Lets tests/ops produce partial failures
    // without root or full disks. Empty in production.
    std::function<bool(const Path&)> fault_injector;
};

// Runs one request to completion on the calling thread. `resolve` is called for every
// Conflict under ConflictPolicy::Ask (an empty resolver answers Skip). `stop` cancels (S9).
// Validation as IOperationQueue::submit (S13). Returns the OpResult (its id is 0).
// Errors: InvalidArgument / PermissionDenied (request rejected, nothing ran: S3, S13),
// NoSpace (S14, nothing written), OutOfMemory.
[[nodiscard]] Result<OpResult> run_operation(const OpRequest& request, const EngineOptions& options,
                                             const ConflictResolver& resolve,
                                             const ProgressSink& progress, std::stop_token stop);

// Events delivered by IOperationQueue::poll, in the order they happened per operation.
struct OpEvent {
    enum class Type : std::uint8_t {
        Started = 0,        // left the queue; `progress` is the first snapshot
        Progress = 1,       // coalesced: at most one per operation per poll
        ConflictRaised = 2, // `conflict` is set; the operation waits for resolve()
        ItemFinished = 3,   // `item` is set (top-level items only: the Inspector follows moves)
        Finished = 4,       // `result` is set; the operation is gone from snapshot()
    };
    Type type = Type::Progress;
    OpId op;
    std::optional<OpProgress> progress;
    std::optional<Conflict> conflict;
    std::optional<ItemOutcome> item;
    std::shared_ptr<const OpResult> result;
};

struct QueueOptions {
    EngineOptions engine;
    // Operations that may run at the same time. Two operations whose path sets overlap
    // (a source, the destination, or a trashed item's files path of one is_same_or_inside a
    // path of the other, either way) never run concurrently: the later one stays Queued until
    // the earlier one finishes (submission order). An operation waiting for a conflict
    // decision or paused keeps its slot. Range 1..8; 0 means 2.
    std::size_t max_concurrent = 2;
    // Called on a worker thread after an event is queued for poll(). Must be thread-safe,
    // cheap, non-blocking and must not call back into the queue (the ui pushes an SDL user
    // event to wake the frame loop).
    std::function<void()> on_event;
};

// Background operation queue. See "Thread-safety" in the header comment.
class IOperationQueue {
public:
    IOperationQueue() = default;
    IOperationQueue(const IOperationQueue&) = delete;
    IOperationQueue& operator=(const IOperationQueue&) = delete;
    IOperationQueue(IOperationQueue&&) = delete;
    IOperationQueue& operator=(IOperationQueue&&) = delete;
    // Implementations call shutdown() from their destructor.
    virtual ~IOperationQueue() = default;

    // Validates (lexically, S3, S13) and queues. Never blocks on I/O.
    // Errors: InvalidArgument (empty sources, Rename without exactly one source, invalid
    // new_name, into itself, relative paths, Undo/Redo without record, DeletePermanently
    // without `confirmed`), PermissionDenied (protected source, S13), Cancelled (after
    // shutdown()).
    [[nodiscard]] virtual Result<OpId> submit(OpRequest request) = 0;

    // Answers a pending conflict. Errors: NotFound (unknown or already answered id; the
    // operation was cancelled), InvalidArgument (action not allowed by the Conflict, invalid
    // new_name).
    [[nodiscard]] virtual Result<void> resolve(ConflictId conflict, ConflictDecision decision) = 0;

    // Cancels a queued or running operation (S9); a pending conflict of it is withdrawn. The
    // Finished event follows. Unknown or finished ids: no-op.
    virtual void cancel(OpId op) noexcept = 0;
    virtual void cancel_all() noexcept = 0;

    // Pause stops at the next check (as cancel) and holds; resume continues. No-ops for
    // unknown, queued-only or finished ids.
    virtual void pause(OpId op) noexcept = 0;
    virtual void resume(OpId op) noexcept = 0;

    // Moves queued events into `out` (appending), oldest first. Returns how many.
    virtual std::size_t poll(std::vector<OpEvent>& out) = 0;

    // Latest progress of every unfinished operation, submission order.
    [[nodiscard]] virtual std::vector<OpProgress> snapshot() const = 0;

    // Pending conflicts not yet answered, raise order (the ui shows the first one).
    [[nodiscard]] virtual std::vector<Conflict> pending_conflicts() const = 0;

    // Replaces EngineOptions::protected_paths for operations submitted from now on.
    virtual void set_protected_paths(ProtectedPaths paths) = 0;

    // Cancels everything and joins the workers (each stops within one chunk; a worker stuck
    // in a blocking call on a hung network mount is detached after 2 s). Pending Finished
    // events stay pollable. Idempotent. The ui calls it on quit after asking the user when
    // operations are still running.
    virtual void shutdown() noexcept = 0;
};

// Starts the workers. Errors: InvalidArgument (max_concurrent > 8), OutOfMemory,
// PlatformError (thread creation failed).
[[nodiscard]] Result<std::unique_ptr<IOperationQueue>> create_operation_queue(QueueOptions options);

// ---------------------------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kDefaultUndoDepth = 100;

// Bounded undo/redo history of finished operations. Runtime only: NOT persisted across
// restarts in Phase 7 (a record refers to inode identities and trash names that another
// program may change while Navigator is closed). One stack per App (all windows, all panes).
// UI thread only.
//
// Flow: the ui passes every OpResult::record it gets (Finished event) to record_done() with
// the operation's origin; edit.undo takes the newest record with take_undo() and submits
// OpRequest{kind = Undo, origin = Undo, record}; edit.redo likewise with take_redo(). A taken
// record is "in flight": it is in neither list; the operation's own result record comes back
// through record_done(). If the undo operation fails as a whole (submit error), the ui puts
// the record back with restore().
class UndoStack {
public:
    explicit UndoStack(std::size_t depth = kDefaultUndoDepth) noexcept;

    // origin User: push onto the undo list and clear the redo list. origin Undo: push onto the
    // redo list. origin Redo: push onto the undo list, redo list kept. Assigns a fresh UndoId.
    // The oldest undo entry is dropped beyond `depth`. Returns the assigned id.
    UndoId record_done(UndoRecord record, OpOrigin origin);

    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    // Newest entry, or nullptr (for the palette label "Undo: move 3 items to Photos").
    [[nodiscard]] const UndoRecord* peek_undo() const noexcept;
    [[nodiscard]] const UndoRecord* peek_redo() const noexcept;

    // Removes and returns the newest entry (nullptr when empty).
    [[nodiscard]] std::shared_ptr<const UndoRecord> take_undo();
    [[nodiscard]] std::shared_ptr<const UndoRecord> take_redo();

    // Puts a taken record back on the side it came from (submit failed).
    void restore(std::shared_ptr<const UndoRecord> record, OpOrigin taken_for);

    void clear() noexcept;
    [[nodiscard]] std::size_t undo_size() const noexcept;
    [[nodiscard]] std::size_t redo_size() const noexcept;
    [[nodiscard]] std::size_t depth() const noexcept;

private:
    std::size_t depth_ = kDefaultUndoDepth;
    std::uint64_t next_id_ = 1;
    std::vector<std::shared_ptr<const UndoRecord>> undo_; // oldest first
    std::vector<std::shared_ptr<const UndoRecord>> redo_; // oldest first
};

} // namespace navigator::ops
