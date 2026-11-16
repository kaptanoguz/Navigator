// navigator/fs.hpp
//
// Purpose: directory listing, metadata and sorting. The UI never walks the filesystem with
//          std::filesystem directly; it goes through navigator::fs (and later navigator::IIndex).
// Owner:   contracts (header); implementation in src/core (role: core-fs). STUB until Phase 1.
// Thread-safety: free functions are reentrant and may be called from worker threads; they share
//                no mutable state. Listing is blocking I/O: never call it on the UI thread.
#pragma once

#include <cstdint>
#include <vector>

#include <navigator/types.hpp>

namespace navigator::fs {

enum class EntryKind : std::uint8_t {
    Unknown,
    File,
    Directory,
    Symlink,
    Fifo,
    Socket,
    BlockDevice,
    CharDevice,
};

struct DirEntry {
    Path name; // file name only (no directory part), raw bytes
    EntryKind kind = EntryKind::Unknown;
    // Symlink only: kind of the target; Unknown if the link is broken.
    EntryKind target_kind = EntryKind::Unknown;
    std::uint64_t size = 0;    // bytes; 0 for directories
    std::int64_t mtime_ns = 0; // modification time, ns since Unix epoch
    std::uint32_t mode = 0;    // POSIX st_mode permission/type bits
    FileId id;
    bool hidden = false; // dot-file (later: also .hidden list)
};

enum class SortKey : std::uint8_t { Name, Size, Modified, Kind, Extension };

struct ListOptions {
    bool include_hidden = false;
    bool follow_symlinks = false; // stat the target for target_kind/size
    SortKey sort = SortKey::Name;
    bool descending = false;
    bool directories_first = true;
};

// Phase 1. Errors: NotFound, PermissionDenied, InvalidArgument (not a directory), IoError,
// Cancelled (later, via a cancellation token overload).
[[nodiscard]] Result<std::vector<DirEntry>> list_directory(const Path& dir,
                                                           const ListOptions& options);

// Phase 1. Metadata of one path (lstat semantics unless follow_symlinks).
[[nodiscard]] Result<DirEntry> stat(const Path& path, bool follow_symlinks);

// Phase 1. Sorts in place by options.sort / descending / directories_first (natural name order).
void sort_entries(std::vector<DirEntry>& entries, const ListOptions& options);

} // namespace navigator::fs
