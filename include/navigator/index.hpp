// navigator/index.hpp
//
// Purpose: Phase 5 persistent file-NAME index of a local volume (a Navigator extra, see
//          docs/features.md "Navigator extras"): built by an incremental, resumable walk, kept
//          current with inotify plus directory-mtime rescans, saved to a versioned binary file
//          in the cache dir, and queried with the matching model of search.hpp. It serves the
//          flattened view of large trees ("whole drive", Ctrl+Shift+E) instantly once ready.
//          Names only: no file contents (Phase 10), no metadata beyond kind.
// Owner:   contracts (header); implementation in src/search (role: search), tests in
//          tests/search. src/search links navigator_core (walk.hpp, fold) and uses watch.hpp only
//          through the injected WatcherFactory (no src/platform calls).
// Thread-safety: an IIndex owns ONE internal service thread (started by start()) that loads,
//                scans, watches and saves. query() and status() may be called from any thread,
//                concurrently with each other and with the service thread; queries see a
//                consistent snapshot (updates are applied in short exclusive sections, at most
//                one directory's worth of changes each, so a query waits at most about a
//                millisecond for a writer). start(), stop() and request_rescan() are called
//                from one owning thread (the UI thread in the app). The inline helpers at the
//                end are pure.
// Errors:  navigator::Result (types.hpp). Background failures never throw or abort: they are
//          reported in IndexStatus::last_error and the state machine below.
//
// =============================================================================================
// Lifecycle
// =============================================================================================
//
//   Idle --start()--> Loading --file ok--> Verifying --done--> Ready <--> (live updates)
//                        |                                       ^
//                        +--no file / corrupt / other volume--> Scanning --done--+
//   any --root unreadable or gone--> Failed (retried at the next rescan interval)
//   any --stop()--> Stopped (saves first when dirty and a file is configured)
//
// Loading:   reads options.file (see "File format"); rejects it (deletes it, logs the reason in
//            last_error, full scan) when the magic, version, size, bounds or checksum are wrong,
//            or when the stored VolumeIdentity differs from options.identity.
// Scanning:  full breadth-first walk of the root (fs::Walker: metadata false, show_hidden true,
//            follow_symlinks false, cross_filesystems false). Hidden entries ARE indexed; queries
//            apply the hidden policy. Resumable: stopping mid-scan saves the partial index with
//            the Complete flag clear, and the next start() resumes with the directories that are
//            not marked Scanned (no re-walk of finished directories).
// Verifying: the cache is loaded and queryable; the service thread fstatat()s every indexed
//            directory and re-lists only those whose mtime differs from the stored one, or is
//            within 2 s of the stored scan start (coarse timestamp guard). A directory's mtime
//            changes when entries are created, deleted or renamed in it, so this costs one stat
//            per DIRECTORY instead of one getdents + stat per entry: this is why a restart is
//            faster than a cold walk (acceptance; measured in docs/perf/phase5-search.md).
// Ready:     live updates (below); a periodic Verifying pass every rescan_interval covers
//            directories without a watch; saves at most every save_interval while dirty.
//
// Live updates (inotify through options.watcher_factory)
//   - inotify watches are per directory and non-recursive, and the per-user limit
//     (fs.inotify.max_user_watches: 8192 on older kernels, RAM-scaled up to ~1M since 5.11) is
//     shared with every other program and with Navigator's own pane watchers. Each index
//     therefore watches at most options.watch_budget directories, assigned in breadth-first
//     order (shallow directories change most often for users). A PlatformError from
//     IWatcher::add() (limit reached) stops adding watches for this index
//     (IndexStatus::watch_budget_exhausted).
//   - Directories without a watch are recorded (directory flag Unwatched, counted in
//     IndexStatus::dirs_unwatched) and are only refreshed by the periodic Verifying pass, so
//     their changes may show up to rescan_interval late. watch_budget 0 or an empty factory
//     disables inotify entirely (periodic passes only).
//   - Created / MovedTo add the entry (a new directory is listed and watched, budget
//     permitting); Deleted / MovedFrom remove it (with its subtree); Renamed renames in place;
//     Modified and Attrib are ignored (names only); WatchedDirGone removes that subtree;
//     Overflow schedules an immediate Verifying pass.
//   - Coverage limits are watch.hpp's: changes made by other machines on network mounts are
//     invisible, which is one reason network volumes are never indexed by default.
//
// Memory budget: about 50-70 bytes per entry in memory (raw name and folded name in string
// pools, a 16-byte entry record, directory records), i.e. roughly 60 MB per million entries; the
// file is about 20 + name bytes per entry. At max_entries the index stops adding entries
// (IndexStatus::truncated) and keeps serving what it has.
//
// Which volumes (should_index_by_default below): local Root, Home and Internal (read-write)
// volumes only; never Network, Removable, Virtual or Other volumes unless the user opts in
// (the opt-in setting is Phase 9; until then only the defaults are indexed).
// AppConfig::enable_index = false (`--no-index`) disables indexing entirely.
//
// Privacy: the file lists every file name on the volume. Directories are created 0700, files
// written 0600, as for the session.
//
// =============================================================================================
// File format (version 1)
// =============================================================================================
//
// Location: <cache root>/index/<index_stable_id(identity)>.idx where <cache root> is
//   platform::user_dir(Cache) / "navigator" (~/.cache/navigator; honours $XDG_CACHE_HOME), or
//   AppConfig::cache_dir. index_file() builds it.
// Writes are atomic (unique temporary file in the same directory, fsync, rename over the old
// file), as layout::save_session. All integers little-endian, offsets from the file start.
//
//   Header, 96 bytes
//     0   u8[8]  magic 4E 41 56 49 44 58 00 0A ("NAVIDX\0\n")
//     8   u32    format version = kIndexFormatVersion (1); any other value: rebuild
//     12  u32    flags: bit 0 Complete (scan finished), bit 1 Truncated; other bits 0
//     16  i64    saved_at, ns since the Unix epoch (UTC)
//     24  i64    scan_started_at, ns: start of the last full scan or verify pass
//     32  u64    dir_count      (>= 1: record 0 is the root)
//     40  u64    entry_count
//     48  u64    pool_bytes
//     56  u64    dirs_offset    (multiple of 8)
//     64  u64    entries_offset (multiple of 8)
//     72  u64    pool_offset
//     80  u32    volume_bytes   (size of the volume block at offset 96)
//     84  u32    reserved, 0
//     88  u64    reserved, 0
//   Volume block: five strings, each a u32 byte length followed by the raw bytes: stable id,
//     uuid, device, mount point, subvolume root (VolumeIdentity fields; checked on load).
//   Directory records, dir_count x 48 bytes:
//     u32 parent       directory index of the parent; 0xFFFFFFFF for the root
//     u32 self_entry   index of this directory's entry record; 0xFFFFFFFF for the root
//     u32 first_child  index of the first child entry (children of one directory are
//                      contiguous in the entry table)
//     u32 child_count
//     i64 mtime_ns     directory mtime when it was last read
//     u64 device       st_dev
//     u64 inode        st_ino
//     u32 flags        bit 0 Scanned, bit 1 Unreadable (EACCES), bit 2 OtherFs (mount point,
//                      not entered), bit 3 Hidden (the directory or an ancestor below the root
//                      is hidden), bit 4 Unwatched (no inotify watch at save time)
//     u32 reserved, 0
//   Entry records, entry_count x 16 bytes:
//     u32 name_offset  into the pool
//     u16 name_len     1..65535 bytes, never containing '/' or NUL
//     u8  kind         fs::EntryKind value (0..4)
//     u8  flags        bit 0 hidden name, bit 1 symlink whose target is a directory (known
//                      only when the scan stat'ed it; else 0)
//     u32 parent       directory index
//     u32 dir          directory index when this entry is a directory that was entered,
//                      else 0xFFFFFFFF
//   Name pool: pool_bytes of raw names, concatenated, no separators.
//   Trailer: u64 FNV-1a 64-bit hash (offset basis 0xcbf29ce484222325, prime 0x100000001b3) of
//     every preceding byte of the file.
// Folded names are NOT stored: they are recomputed at load time, so the file does not depend on
// the Unicode version of the fold tables. A file larger than kMaxIndexFileBytes, any record or
// string out of bounds, a cycle in parent links or a checksum mismatch is CORRUPT: the index
// deletes the file and rebuilds from scratch (never a crash on hostile input).
// Versioning rule: an incompatible change bumps kIndexFormatVersion; older files are then
// rebuilt (an index is a cache: no migration).
#pragma once

#include <array>
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
#include <navigator/search.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>
#include <navigator/watch.hpp>

namespace navigator::search {

inline constexpr std::uint32_t kIndexFormatVersion = 1;
inline constexpr std::string_view kIndexDirName = "index";
inline constexpr std::string_view kIndexFileSuffix = ".idx";
inline constexpr std::uint64_t kMaxIndexFileBytes = std::uint64_t{2} * 1024U * 1024U * 1024U;

inline constexpr std::size_t kDefaultWatchBudget = 8192;
inline constexpr std::size_t kDefaultIndexMaxEntries = 2'000'000;
inline constexpr std::size_t kDefaultIndexQueryLimit = 1000;
inline constexpr std::chrono::seconds kDefaultRescanInterval{15 * 60};
inline constexpr std::chrono::seconds kDefaultSaveInterval{5 * 60};

enum class IndexState : std::uint8_t {
    Idle = 0,      // created, start() not called
    Loading = 1,   // reading the index file
    Scanning = 2,  // full walk (no usable file); query() returns partial results
    Verifying = 3, // file loaded; re-checking directory mtimes; query() works on loaded data
    Ready = 4,     // complete and live
    Failed = 5,    // root unreadable or gone; last_error says why; retried every rescan interval
    Stopped = 6,   // stop() was called
};

[[nodiscard]] constexpr std::string_view to_string(IndexState state) noexcept {
    switch (state) {
    case IndexState::Idle: return "idle";
    case IndexState::Loading: return "loading";
    case IndexState::Scanning: return "scanning";
    case IndexState::Verifying: return "verifying";
    case IndexState::Ready: return "ready";
    case IndexState::Failed: return "failed";
    case IndexState::Stopped: return "stopped";
    }
    return "unknown";
}

// Snapshot for the ui (status bar / sidebar progress). Cheap to copy.
struct IndexStatus {
    IndexState state = IndexState::Idle;
    std::uint64_t entries = 0;        // indexed entries (grows while Scanning)
    std::uint64_t directories = 0;    // directory records
    std::uint64_t dirs_pending = 0;   // directories queued for the current scan/verify pass
    std::uint64_t dirs_unwatched = 0; // directories without an inotify watch
    std::uint64_t skipped_permission = 0;
    bool live = false; // inotify is delivering updates
    bool watch_budget_exhausted = false;
    bool truncated = false;         // reached max_entries
    bool loaded_from_file = false;  // this run started from the index file
    Timestamp last_complete_scan{}; // end of the last complete scan or verify; {} if none
    std::chrono::milliseconds load_duration{0}; // file read + parse + fold (benchmark)
    std::chrono::milliseconds scan_duration{0}; // last full scan or verify pass (benchmark)
    std::optional<Error> last_error; // most recent background problem (file rejected, ...)
    // Bumped whenever query results may differ; the ui re-runs a visible index query when it
    // changes (throttled).
    std::uint64_t generation = 0;
};

// Verify: re-check directory mtimes now (cheap). Rebuild: discard everything and walk the
// whole volume again (search.rebuild_index).
enum class RescanKind : std::uint8_t {
    Verify = 0,
    Rebuild = 1,
};

struct IndexQuery {
    Query query; // compile_query(); empty: every entry under `under`
    Path under;  // absolute; must be the index root or below it; empty = root
    TypeFilter types;
    bool show_hidden = false; // false: hidden entries and everything below hidden
                              // directories (relative to `under`) are excluded, as in a walk
    std::size_t limit = kDefaultIndexQueryLimit; // 0 = no limit
};

struct IndexHit {
    Path path; // absolute
    fs::EntryKind kind = fs::EntryKind::Unknown;
    bool directory_like = false; // Directory, or a symlink known to point to one
    std::uint32_t depth = 1;     // relative to IndexQuery::under (1 = its child)
    Match match;                 // score and highlights within the name (last component)
};

struct IndexResult {
    std::vector<IndexHit> hits;   // best first: ranks_before() with parent relative to `under`
    std::uint64_t matched = 0;    // matches before applying the limit
    bool complete = false;        // the index was Ready when the query ran
    std::uint64_t generation = 0; // IndexStatus::generation the result reflects
};

class IIndex {
public:
    IIndex() = default;
    IIndex(const IIndex&) = delete;
    IIndex& operator=(const IIndex&) = delete;
    IIndex(IIndex&&) = delete;
    IIndex& operator=(IIndex&&) = delete;
    // Implementations call stop() from their destructor.
    virtual ~IIndex() = default;

    // Starts the service thread (Loading). Second and later calls are no-ops.
    virtual void start() = 0;
    // Requests stop, saves when dirty (file configured), joins the service thread. Returns
    // promptly (stop checks as in walk.hpp) unless a filesystem call blocks; only local
    // volumes are indexed, so that is not expected. Idempotent.
    virtual void stop() noexcept = 0;
    // Queues a pass; coalesces with a pending one (Rebuild wins). No-op unless started.
    virtual void request_rescan(RescanKind kind) = 0;

    [[nodiscard]] virtual IndexStatus status() const = 0;
    // The indexed volume's mount point (NameIndexOptions::root).
    [[nodiscard]] virtual const Path& root() const noexcept = 0;

    // Matching, hidden policy and type filter as in search.hpp. Usable in every state except
    // Idle/Failed-without-data (then an empty, incomplete result). The stop token is checked at
    // least every kFilterStopCheckInterval entries. Target: < 100 ms for 1M entries and a
    // 3-character query in a Release build (benchmark).
    // Errors: InvalidArgument (`under` relative, or not the root or below it), Cancelled,
    // OutOfMemory.
    [[nodiscard]] virtual Result<IndexResult> query(const IndexQuery& query,
                                                    std::stop_token stop) const = 0;
};

struct NameIndexOptions {
    Path root;                      // absolute mount point of the volume
    VolumeIdentity identity;        // stored in the file and compared on load
    Path file;                      // index file (index_file()); empty: in-memory only (tests)
    WatcherFactory watcher_factory; // empty: no inotify, periodic passes only
    std::size_t watch_budget = kDefaultWatchBudget;
    std::chrono::seconds rescan_interval = kDefaultRescanInterval; // 0: no periodic pass
    std::chrono::seconds save_interval = kDefaultSaveInterval;
    std::size_t max_entries = kDefaultIndexMaxEntries;
    // Best effort: the service thread lowers its own CPU and I/O priority so a scan of / does
    // not compete with the UI and pane listings.
    bool background_priority = true;
    // Called on the service thread after every status change that the ui may show (state,
    // every few thousand entries of progress, generation). Must be thread-safe, cheap and
    // non-blocking, and must not call back into the index; the ui pushes an SDL user event.
    std::function<void()> on_status_change;
};

// Creates the default implementation (not started).
// Errors: InvalidArgument (root empty or relative, max_entries 0), OutOfMemory.
[[nodiscard]] Result<std::unique_ptr<IIndex>> create_name_index(NameIndexOptions options);

// ---------------------------------------------------------------------------------------------
// Pure helpers (inline)
// ---------------------------------------------------------------------------------------------

// Default index policy: local, fixed, writable volumes the user browses. A read-only Root
// (image-based distributions) is still indexed. Removable, Network, Virtual and Other volumes
// are never indexed by default; neither are image filesystems mounted as Internal.
[[nodiscard]] inline bool should_index_by_default(const VolumeInfo& volume) noexcept {
    if (volume.removable) {
        return false;
    }
    switch (volume.kind) {
    case VolumeKind::Root: return true;
    case VolumeKind::Home:
    case VolumeKind::Internal: break;
    case VolumeKind::Removable:
    case VolumeKind::Network:
    case VolumeKind::Virtual:
    case VolumeKind::Other: return false;
    }
    if (volume.read_only) {
        return false;
    }
    constexpr std::array<std::string_view, 4> kImageFs{"squashfs", "iso9660", "udf", "erofs"};
    for (const std::string_view fs_type : kImageFs) {
        if (volume.fs_type == fs_type) {
            return false;
        }
    }
    return true;
}

// FNV-1a 64-bit (the file trailer and stable ids use it).
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view bytes,
                                              std::uint64_t hash = 0xcbf29ce484222325ULL) noexcept {
    for (const char c : bytes) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// File-name-safe stable id of a volume, independent of where it is mounted when a uuid is known:
//   uuid known:  "u-" + sanitized uuid, plus "-" + 16 hex digits of fnv1a64(root) when the
//                subvolume root is neither empty nor "/" (btrfs subvolumes of one filesystem
//                get distinct indexes);
//   otherwise:   "d-" + 16 hex digits of fnv1a64(device + "\n" + mount point bytes).
// sanitized: ASCII letters lowercased, digits and '-' kept, every other byte becomes '_', cut to
// 64 bytes.
[[nodiscard]] inline std::string index_stable_id(const VolumeIdentity& identity) {
    constexpr std::string_view kHex = "0123456789abcdef";
    const auto hex16 = [&](std::uint64_t value) {
        std::string out(16, '0');
        for (std::size_t i = 0; i < 16; ++i) {
            out[15 - i] = kHex[value & 0xFU];
            value >>= 4U;
        }
        return out;
    };
    std::string id;
    if (!identity.uuid.empty()) {
        id = "u-";
        for (const char c : identity.uuid) {
            if (id.size() >= 66) {
                break;
            }
            if (c >= 'A' && c <= 'Z') {
                id.push_back(static_cast<char>(c - 'A' + 'a'));
            } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
                id.push_back(c);
            } else {
                id.push_back('_');
            }
        }
        if (!identity.root.empty() && identity.root != "/") {
            id += '-';
            id += hex16(fnv1a64(identity.root));
        }
        return id;
    }
    std::string key = identity.device;
    key += '\n';
    key += identity.mount_point.native();
    id = "d-";
    id += hex16(fnv1a64(key));
    return id;
}

// <cache_root>/index; cache_root is platform::user_dir(Cache) / "navigator" or
// AppConfig::cache_dir.
[[nodiscard]] inline Path index_dir(const Path& cache_root) { return cache_root / kIndexDirName; }

[[nodiscard]] inline Path index_file(const Path& cache_root, const VolumeIdentity& identity) {
    std::string name = index_stable_id(identity);
    name += kIndexFileSuffix;
    return index_dir(cache_root) / name;
}

} // namespace navigator::search
