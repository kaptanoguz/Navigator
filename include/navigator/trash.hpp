// navigator/trash.hpp
//
// Purpose: the FreeDesktop.org Trash specification 1.0 (home trash, per-volume trashes,
//          .trashinfo files): pure format helpers and the ITrash service that moves one item
//          into a trash, lists trashed items, puts one back and forgets its info file.
//          Phase 7. Byte copying, recursive deletion and conflict handling are NOT here: they
//          belong to the ops engine (ops.hpp), which uses ITrash for the same-filesystem steps.
// Owner:   contracts (header); implementation in src/platform (role: platform), tests in
//          tests/platform (fake home data dir, fake mount table through VolumeQuery).
// Thread-safety: the free functions are pure and callable from any thread. An ITrash made by
//                create_trash() holds only its immutable options: every member may be called
//                concurrently from several worker threads (unique trash names are reserved with
//                O_EXCL, so two concurrent trash() calls never pick the same name). Members block
//                on file I/O (and on statvfs / mount table reads): never call them on the UI
//                thread; the ops queue and the ui's trash-listing job call them on workers.
//                Exception: is_inside_trash() is lexical and may run on the UI thread.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
//
// Normative rules (docs/architecture.md "Phase 7", "Safety rules"):
//   T1. Trashing is a rename(2) within one filesystem. An item is NEVER copied into a trash on
//       another filesystem (the spec allows it; Navigator refuses). When no trash on the item's
//       own filesystem is usable, trash() fails with Unsupported and changes nothing; the ui
//       then offers "Delete permanently" through its separate, confirmed command. Nothing in
//       this header deletes file data.
//   T2. The item itself is trashed: a symlink is moved as a link, never followed.
//   T3. Home trash: <home_trash> = $XDG_DATA_HOME/Trash (default ~/.local/share/Trash), with
//       files/ and info/ created 0700 when missing. Used for items on the same filesystem
//       (st_dev) as <home_trash>. Its .trashinfo Path is the absolute original path.
//   T4. Other filesystems: <topdir> = mount point of the volume holding the item (VolumeQuery
//       mount table). First choice $topdir/.Trash/$uid, used only when $topdir/.Trash exists,
//       is a directory, is NOT a symlink and has the sticky bit (spec "(1)"); a .Trash that
//       fails a check is ignored (never used, never "fixed"). Second choice $topdir/.Trash-$uid
//       (spec "(2)"), created 0700 when missing; an existing one must be a directory, not a
//       symlink, owned by uid, else that volume is Unsupported. The uid directory below .Trash
//       obeys the same owner / not-a-symlink rule. Their .trashinfo Path is relative to
//       <topdir> (spec: "relative to the directory in which the trash directory resides").
//       Network filesystems follow the same rules (a writable NFS export gets .Trash-$uid);
//       read-only volumes fail with ReadOnlyFilesystem. GVfs locations are not handled here.
//   T5. Name reservation: info/<name>.trashinfo is created with O_CREAT|O_EXCL (mode 0600)
//       BEFORE the item is renamed into files/<name>; on EEXIST the next candidate is tried:
//       <name>, <name>.2, <name>.3, ... (GIO style). Names are cut at a UTF-8 code point
//       boundary so that "<name>.trashinfo" fits NAME_MAX bytes. If the rename fails, the info
//       file is unlinked again (the only unlink in this module apart from forget()).
//   T6. The directorysizes cache (spec 1.0, optional) is neither read nor written. Readers
//       validate its entries against the info file's mtime, so stale entries are harmless.
//   T7. Refused sources (InvalidArgument, nothing changes): relative or empty paths, "/", a
//       mount point (st_dev differs from its parent's), a trash directory or anything inside
//       one (use the trash view's permanent delete instead), and the home trash's ancestors.
#pragma once

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
#include <navigator/types.hpp>
#include <navigator/volume.hpp>

namespace navigator::trash {

// ---------------------------------------------------------------------------------------------
// Pure format helpers
// ---------------------------------------------------------------------------------------------

// Wall-clock local time without a zone, as the spec's DeletionDate ("YYYY-MM-DDThh:mm:ss",
// local time). Plain value; no validation beyond what parse_deletion_date() does.
struct LocalDateTime {
    int year = 1970;
    int month = 1; // 1..12
    int day = 1;   // 1..31
    int hour = 0;
    int minute = 0;
    int second = 0;

    friend constexpr bool operator==(const LocalDateTime&, const LocalDateTime&) = default;
    friend constexpr auto operator<=>(const LocalDateTime&, const LocalDateTime&) = default;
};

// "YYYY-MM-DDThh:mm:ss", zero padded (four-digit year). Pure.
[[nodiscard]] std::string format_deletion_date(const LocalDateTime& when);

// Inverse of format_deletion_date(). Lenient like GIO and KDE: a trailing fraction (".123")
// and a trailing zone ("Z", "+03:00") are accepted and ignored; ranges are checked (month
// 1..12, day 1..31, hour 0..23, minute 0..59, second 0..60). nullopt for anything else. Pure.
[[nodiscard]] std::optional<LocalDateTime> parse_deletion_date(std::string_view text);

// Percent-encoding of the Path key, exactly as GIO writes it (g_uri_escape_string(path, "/",
// FALSE)): bytes in [A-Za-z0-9] and - . _ ~ / are kept; every other byte (including every byte
// >= 0x80, so UTF-8 is encoded byte by byte) becomes %XX with UPPERCASE hex.
// Example: "/home/oğuz/a b.txt" -> "/home/o%C4%9Fuz/a%20b.txt". Pure.
[[nodiscard]] std::string escape_path(std::string_view raw);

// Inverse: %XX in either case decoded, other bytes kept (KDE leaves some characters raw).
// nullopt on a malformed escape ("%4", "%G1") or an escaped NUL ("%00"). Pure.
[[nodiscard]] std::optional<std::string> unescape_path(std::string_view escaped);

// Contents of one .trashinfo file. `path` holds the DECODED raw bytes as written in the file:
// absolute for the home trash, relative to the topdir for per-volume trashes (T3, T4).
struct TrashInfo {
    std::string path;
    std::optional<LocalDateTime> deletion_date; // nullopt: missing or unparsable (still listed)

    friend bool operator==(const TrashInfo&, const TrashInfo&) = default;
};

// "[Trash Info]\nPath=<escape_path(path)>\nDeletionDate=<date>\n" (DeletionDate omitted when
// nullopt; Navigator always writes it). Pure.
[[nodiscard]] std::string format_trashinfo(const TrashInfo& info);

// Parses a .trashinfo file: the first "[Trash Info]" group is used (lines before it and other
// groups are ignored), keys Path and DeletionDate (first occurrence wins), other keys ignored,
// "\n" or "\r\n" line ends, no trailing newline needed. Errors: InvalidArgument (no
// [Trash Info] group, Path missing, empty or not unescapable), OutOfMemory. Pure.
[[nodiscard]] Result<TrashInfo> parse_trashinfo(std::string_view text);

// $topdir/.Trash/<uid> and $topdir/.Trash-<uid> (decimal uid). Pure; no checks.
[[nodiscard]] Path shared_topdir_trash(const Path& topdir, std::uint32_t uid);
[[nodiscard]] Path user_topdir_trash(const Path& topdir, std::uint32_t uid);

// Value written as Path= for `original` (absolute, lexically normal): `original` itself for a
// home trash (topdir empty), else `original` relative to `topdir`. InvalidArgument when
// `original` is not below `topdir`. Pure.
[[nodiscard]] Result<std::string> info_path_for(const Path& original, const Path& topdir);

// Original absolute path from a Path= value: an absolute value is used as is (also in a
// topdir trash, as the spec allows); a relative one is joined to `topdir`. The result is
// lexically normal; a relative value that escapes the topdir (".." past it) or any value in a
// home trash that is relative is InvalidArgument. Pure.
[[nodiscard]] Result<Path> original_path_from(std::string_view info_path, const Path& topdir);

// ---------------------------------------------------------------------------------------------
// Trash directories and items
// ---------------------------------------------------------------------------------------------

enum class TrashDirKind : std::uint8_t {
    Home = 0,       // $XDG_DATA_HOME/Trash
    SharedUser = 1, // $topdir/.Trash/$uid   (spec method 1)
    User = 2,       // $topdir/.Trash-$uid   (spec method 2)
};

struct TrashDir {
    TrashDirKind kind = TrashDirKind::Home;
    Path root;                // the directory that holds files/ and info/
    Path topdir;              // mount point the relative Path= values refer to; empty for Home
    std::uint64_t device = 0; // st_dev of root (items on this device go here)

    [[nodiscard]] Path files_dir() const { return root / "files"; }
    [[nodiscard]] Path info_dir() const { return root / "info"; }

    friend bool operator==(const TrashDir&, const TrashDir&) = default;
};

// One trashed item: files/<name> plus info/<name>.trashinfo.
struct TrashedItem {
    TrashDir dir;
    std::string name;   // entry name in files/ (raw bytes), unique in `dir`
    Path original_path; // absolute, from the info file (original_path_from)
    std::optional<LocalDateTime> deletion_date;
    // lstat of files/<name> when listed or just trashed (never followed): kind, size (files;
    // 0 for directories, whose tree size the ui computes on demand), mtime, identity.
    fs::EntryKind kind = fs::EntryKind::Unknown;
    std::uint64_t size = 0;
    Timestamp mtime{};
    FileId id;

    [[nodiscard]] Path files_path() const { return dir.files_dir() / name; }
    [[nodiscard]] Path info_path() const { return dir.info_dir() / (name + ".trashinfo"); }

    friend bool operator==(const TrashedItem&, const TrashedItem&) = default;
};

struct TrashListing {
    std::vector<TrashedItem> items; // trash_dirs() order, then by deletion date (newest first)
    // Entries skipped: an info file without files/<name>, a files/ entry without an info file
    // (both are "orphans" per spec and not shown), or an unparsable info file.
    std::size_t orphans = 0;
    // A trash directory that could not be read (permission, I/O); the others are still listed.
    std::vector<Error> dir_errors;
};

struct TrashOptions {
    // Home trash root; empty: platform::data_dirs()[0] / "Trash" ($XDG_DATA_HOME/Trash).
    // Tests point it into a temporary directory.
    Path home_trash;
    // Owner of the per-volume trashes; nullopt: the real uid (getuid()).
    std::optional<std::uint32_t> uid;
    // Mount table for topdir lookup and for trash_dirs() (tests inject a fake IMountSource).
    VolumeQuery volumes;
    // T4 per-volume trashes. false: only the home trash (other volumes are Unsupported).
    bool use_topdir_trashes = true;
    // DeletionDate source; empty: the local time of the system clock (localtime_r).
    std::function<LocalDateTime()> now;
};

// The trash service (T1..T7 above).
class ITrash {
public:
    ITrash() = default;
    virtual ~ITrash() = default;
    ITrash(const ITrash&) = delete;
    ITrash& operator=(const ITrash&) = delete;
    ITrash(ITrash&&) = delete;
    ITrash& operator=(ITrash&&) = delete;

    // The trash directory trash(path) would use, without creating or changing anything
    // (directories that trash() would create count as usable when their parent is writable).
    // The ui uses it to label the delete action ("Move to Trash" vs. the permanent-delete
    // sheet) before submitting. Errors: as trash(), but never AlreadyExists.
    [[nodiscard]] virtual Result<TrashDir> trash_dir_for(const Path& path) = 0;

    // Moves `path` into the trash of its own filesystem (T1..T5, T7). On error nothing has
    // changed (an info file reserved in step T5 is removed again).
    // Errors: InvalidArgument (T7), NotFound, PermissionDenied (item's parent or the trash not
    // writable), ReadOnlyFilesystem, Unsupported (no usable trash on that filesystem: T1/T4),
    // NoSpace (info file), IoError, OutOfMemory.
    [[nodiscard]] virtual Result<TrashedItem> trash(const Path& path) = 0;

    // Existing, valid trash directories: the home trash (even when it does not exist yet, so
    // the ui can watch for its creation) then, when use_topdir_trashes, the SharedUser / User
    // trashes found on the mounted volumes of the mount table (volume list order). Never
    // creates anything. Errors: those of list_volumes(); unreadable volumes are skipped.
    [[nodiscard]] virtual Result<std::vector<TrashDir>> trash_dirs() = 0;

    // Every item of every trash_dirs() entry (one lstat per item; no tree walks). Checked for
    // `stop` between items. Errors: Cancelled, those of trash_dirs(), OutOfMemory; per-dir
    // read errors go to TrashListing::dir_errors.
    [[nodiscard]] virtual Result<TrashListing> list(std::stop_token stop) = 0;

    // Same-filesystem restore step: renameat2(files/<name>, destination, RENAME_NOREPLACE),
    // then unlinks the info file. `destination` is normally item.original_path; a "Keep both"
    // decision passes another name in the same folder. The destination's parent must exist
    // (the ops engine creates missing parents first and records them for undo). Before the
    // rename the item is checked against `item` (lstat of files/<name>: same FileId), so a
    // stale listing never restores a different item. Never overwrites.
    // Errors: NotFound (files/<name> or the destination's parent missing), InvalidArgument
    // (identity changed since listing; relative destination), AlreadyExists (destination
    // exists: a conflict for the ops engine), CrossDevice (destination on another filesystem:
    // the ops engine copies and then erases the trashed copy), PermissionDenied,
    // ReadOnlyFilesystem, IoError.
    [[nodiscard]] virtual Result<void> restore(const TrashedItem& item,
                                               const Path& destination) = 0;

    // Removes the info file of an item whose files/<name> entry is already gone (after the ops
    // engine erased it permanently or copied it out). Errors: InvalidArgument (files/<name>
    // still exists: forget() never orphans data), NotFound (info file already gone),
    // PermissionDenied, IoError.
    [[nodiscard]] virtual Result<void> forget(const TrashedItem& item) = 0;

    // True when `path` (lexically normalized) is the home trash or below it, or has a
    // component ".Trash-<uid>", or a component ".Trash" directly followed by "<uid>" (rule T7;
    // also used for the ops engine's protected paths). Lexical only, no I/O: a user folder that
    // happens to be named ".Trash-1000" also matches, which only makes Navigator refuse to
    // trash it (conservative). Callable on the UI thread.
    [[nodiscard]] virtual bool is_inside_trash(const Path& path) const = 0;
};

// Errors: InvalidArgument (relative home_trash), NotFound (home_trash empty and no data dir),
// OutOfMemory.
[[nodiscard]] Result<std::shared_ptr<ITrash>> create_trash(TrashOptions options = {});

} // namespace navigator::trash
