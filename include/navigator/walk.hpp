// navigator/walk.hpp
//
// Purpose: cancellable, streaming, cycle-safe recursive enumeration of a directory tree (Phase 5
//          "flatten", Ctrl+E, and the name index's scans). Enumeration only: matching, type
//          filters and ranking are applied on top by navigator::search (search.hpp).
// Owner:   contracts (header); implementation in src/core (role: core-fs), tests in tests/core.
// Thread-safety: a Walker is NOT thread-safe: one thread at a time (normally the worker that
//                created it; moving it to another thread with ordinary happens-before
//                synchronization is fine). Different Walkers are independent. Every call blocks
//                on file I/O: never create or advance a Walker on the UI thread.
// Errors:  navigator::Result (types.hpp). No exceptions escape; allocation failure is
//          ErrorCode::OutOfMemory.
//
// Model
//   The walk is breadth-first: every entry of depth d is returned before any entry of depth
//   d + 1 (children of the root have depth 1), so shallow results appear first in a flattened
//   view. Within one directory the order is the kernel's readdir order (unspecified); consumers
//   sort or rank.
//   Each returned entry names its parent through an index into the walk's DIRECTORY TABLE
//   instead of a path string (a million entries share a few tens of thousands of parents).
//   Index 0 is the root (relative path ""); every batch carries the directories it added to
//   the table (WalkBatch::new_dirs), and the consumer appends them to its own copy, so an
//   entry's relative path is `dirs[entry.dir] + "/" + entry.entry.name` (no leading "/" for
//   depth-1 entries). Relative paths are raw bytes joined by '/', never normalized.
//
// Descent rules, applied to every entry e of a directory being read:
//   - e is always RETURNED (subject to show_hidden) whether or not it is descended into.
//   - e is DESCENDED INTO when all hold:
//       e.kind == Directory, or follow_symlinks and fs::is_directory_like(e);
//       depth(e) < max_depth;
//       show_hidden, or e is not hidden;
//       cross_filesystems, or e's st_dev equals the root's st_dev (one volume: a mount point
//       below the root is returned as an entry but not entered; WalkStats::skipped_other_fs);
//       e's (st_dev, st_ino) was not entered before in this walk (cycle safety: bind mounts and
//       followed symlinks cannot loop; WalkStats::skipped_cycles).
//   - Hidden policy (File Pilot v0.5 fix per the roadmap): with show_hidden == false hidden
//     entries are not returned AND hidden directories are not entered, so nothing below a
//     hidden directory appears. The root itself is always walked, even when it is hidden (the
//     user is inside ~/.config and presses Ctrl+E). "System" files have no separate flag on
//     Linux; DirEntry::hidden is the policy (a later .hidden-file list extends that flag).
//
// Failures below the root never fail the walk:
//   - EACCES / EPERM opening or reading a subdirectory: counted in skipped_permission (and the
//     path kept in WalkStats::samples) when skip_unreadable; with skip_unreadable == false
//     next_batch() fails with PermissionDenied (the walker is then finished).
//   - ENOENT / ENOTDIR (the directory vanished or was replaced mid-walk): skipped_vanished.
//   - Any other error: skipped_errors (+ sample).
//   - Per-entry stat failures appear as DirEntry::metadata_error, as in fs::list_directory.
//
// Cancellation latency: next_batch() checks the stop token before opening each directory and at
// least every 1024 entries while reading or stat'ing one, and then fails with Cancelled. On a
// local filesystem that bounds the latency to one getdents64 chunk plus at most 1024 fstatat
// calls (single-digit milliseconds). A syscall that blocks (hung network mount, dying disk)
// cannot be interrupted: the caller must never join a walking worker on the UI thread
// (docs/architecture.md "Phase 5"). cross_filesystems == false keeps a walk of a local volume
// off network mounts below it.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::fs {

inline constexpr std::uint32_t kUnlimitedDepth = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t kDefaultWalkBatchSize = 2048;
inline constexpr std::chrono::milliseconds kDefaultWalkBatchLatency{50};
// Flatten keeps every returned entry in memory (about 120-200 bytes each with metadata), so a
// walk stops after this many returned entries (WalkStats::truncated). The ui may lower it.
inline constexpr std::size_t kDefaultWalkMaxEntries = 1'000'000;
// How many skipped-directory paths WalkStats::samples keeps (for a "N folders skipped" tooltip).
inline constexpr std::size_t kWalkErrorSamples = 16;

struct WalkOptions {
    // Absolute directory to walk; a symlink to a directory is followed for the root itself.
    Path root;
    // Deepest entry depth returned (1 = only the root's children, like a listing). Directories
    // at max_depth are returned but not entered.
    std::uint32_t max_depth = kUnlimitedDepth;
    // false: symlinks are returned (kind Symlink) and never entered.
    bool follow_symlinks = false;
    // false: stay on the root's filesystem (one volume; "whole drive" flatten walks from the
    // volume's mount point with this false).
    bool cross_filesystems = false;
    // See "Hidden policy" above.
    bool show_hidden = false;
    // See "Failures below the root" above.
    bool skip_unreadable = true;
    // true: every returned entry has full metadata as fs::list_directory() would give it (one
    // fstatat per entry; the Size / Modified columns of a flattened view need it). false (the
    // name index): only name, kind (from d_type; one fstatat only for DT_UNKNOWN, and for
    // directories, whose st_dev/st_ino the descent rules need), hidden and id are filled; other
    // metadata fields keep their defaults and target_kind stays Unknown.
    bool metadata = true;
    // next_batch() returns once `batch_size` entries are ready, or once `batch_latency` has
    // passed since the call and at least one entry is ready, or at the end of the walk.
    std::size_t batch_size = kDefaultWalkBatchSize;
    std::chrono::milliseconds batch_latency = kDefaultWalkBatchLatency;
    // 0 = no limit. See kDefaultWalkMaxEntries.
    std::size_t max_entries = kDefaultWalkMaxEntries;
};

struct WalkEntry {
    DirEntry entry;          // as fs::list_directory() reports it (see WalkOptions::metadata)
    std::uint32_t dir = 0;   // parent: index into the directory table (0 = root)
    std::uint32_t depth = 1; // 1 = child of the root
    // Set when this entry is a directory that was entered: its index in the directory table
    // (the batch that returns it, or an earlier one, lists it in new_dirs). 0 = not entered.
    std::uint32_t self_dir = 0;
};

struct WalkSkip {
    std::string relative_path; // raw bytes, relative to the root
    ErrorCode code = ErrorCode::Unknown;
};

struct WalkStats {
    std::uint64_t dirs_visited = 0;       // directories fully read (root included)
    std::uint64_t dirs_pending = 0;       // directories queued, not read yet (progress hint)
    std::uint64_t entries_returned = 0;   // entries handed out so far
    std::uint64_t hidden_skipped = 0;     // hidden entries not returned (show_hidden false)
    std::uint64_t skipped_permission = 0; // subdirectories not readable (EACCES / EPERM)
    std::uint64_t skipped_vanished = 0;   // subdirectories gone mid-walk
    std::uint64_t skipped_errors = 0;     // other failures (EIO, ...)
    std::uint64_t skipped_other_fs = 0;   // mount points not entered (cross_filesystems false)
    std::uint64_t skipped_cycles = 0;     // (dev, ino) already entered
    bool truncated = false;               // stopped at max_entries
    bool done = false;                    // the walk finished (completely or truncated)
    std::vector<WalkSkip> samples;        // first kWalkErrorSamples skipped directories
};

struct WalkBatch {
    // Table index of new_dirs[0]; new_dirs[k] has index first_dir + k. Indices are dense and
    // assigned in the order directories are entered; the root (index 0) is never in new_dirs.
    std::uint32_t first_dir = 0;
    std::vector<std::string> new_dirs; // relative paths of directories added by this batch
    std::vector<WalkEntry> entries;

    void clear() noexcept {
        first_dir = 0;
        new_dirs.clear();
        entries.clear();
    }
};

class Walker {
public:
    // Opens the root (one open + fstat; no entries are read yet).
    // Errors: InvalidArgument (empty or relative root, root not a directory, batch_size 0,
    // max_depth 0), NotFound, PermissionDenied (the ROOT is unreadable; never skipped),
    // IoError, OutOfMemory.
    [[nodiscard]] static Result<Walker> open(const WalkOptions& options);

    Walker(Walker&&) noexcept;
    Walker& operator=(Walker&&) noexcept;
    Walker(const Walker&) = delete;
    Walker& operator=(const Walker&) = delete;
    ~Walker();

    // Clears `out`, then fills it with the next entries (and the directories they introduce).
    // Returns true while more may follow, false when this batch is the last one (it may be
    // empty); after false, further calls return false with an empty batch.
    // Errors: Cancelled (stop requested; the walker is finished, `out` holds nothing useful),
    // PermissionDenied (only with skip_unreadable == false), OutOfMemory. After an error the
    // walker is finished; stats() stay readable.
    [[nodiscard]] Result<bool> next_batch(WalkBatch& out, std::stop_token stop);

    [[nodiscard]] const WalkStats& stats() const noexcept;
    [[nodiscard]] const WalkOptions& options() const noexcept;

private:
    struct Impl;
    explicit Walker(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace navigator::fs
