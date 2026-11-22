// src/search/index_tree.hpp (internal to navigator_search)
//
// Purpose: the in-memory model of a name index (navigator/index.hpp) and its file codec.
//          Everything is grouped per directory: a directory owns its children's records and a
//          private name pool holding their raw and folded names, so every live update is "one
//          directory's worth" of work under the index's exclusive lock.
// Thread-safety: plain data. The owning NameIndex guards it with a shared_mutex: the service
//                thread is the only writer (exclusive lock); queries read under a shared lock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>
#include <navigator/watch.hpp>

namespace navigator::search::detail {

inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFU;
// Longest name the index stores (Linux NAME_MAX is 255; the file allows 65535).
inline constexpr std::size_t kMaxIndexedName = 4096;

// Directory record flags (file format "Directory records").
inline constexpr std::uint32_t kDirScanned = 1U << 0U;
inline constexpr std::uint32_t kDirUnreadable = 1U << 1U;
inline constexpr std::uint32_t kDirOtherFs = 1U << 2U;
inline constexpr std::uint32_t kDirHidden = 1U << 3U;
inline constexpr std::uint32_t kDirUnwatched = 1U << 4U;
inline constexpr std::uint32_t kDirKnownFlags = 0x1FU;

// Entry record flags.
inline constexpr std::uint8_t kEntryHidden = 1U << 0U;
inline constexpr std::uint8_t kEntryLinkToDir = 1U << 1U;

// Header flags.
inline constexpr std::uint32_t kFileComplete = 1U << 0U;
inline constexpr std::uint32_t kFileTruncated = 1U << 1U;

struct Child {
    std::uint32_t raw_off = 0;  // into Dir::pool
    std::uint32_t fold_off = 0; // into Dir::pool
    std::uint16_t raw_len = 0;
    std::uint16_t fold_len = 0;
    fs::EntryKind kind = fs::EntryKind::Unknown;
    std::uint8_t flags = 0;
    std::uint32_t dir = kNoIndex; // directory record when this child has one
};

struct Dir {
    bool alive = false;
    std::uint64_t serial = 0; // tells a reused slot from the directory that had it before
    std::uint32_t parent = kNoIndex;
    std::string name;        // raw name of this directory in its parent; empty for the root
    std::string pool;        // raw and folded names of `children`
    std::size_t garbage = 0; // pool bytes no child refers to any more (compacted lazily)
    std::vector<Child> children;
    std::int64_t mtime_ns = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint32_t flags = 0; // kDir* (kDirHidden/kDirUnwatched are computed at save)
    WatchHandle watch = kInvalidWatchHandle; // service thread bookkeeping
};

struct FileIdHash {
    std::size_t operator()(const FileId& id) const noexcept {
        return static_cast<std::size_t>((id.device * 0x9E3779B97F4A7C15ULL) ^ id.inode);
    }
};

struct Tree {
    std::vector<Dir> dirs; // index 0 is the root
    std::vector<std::uint32_t> free_dirs;
    std::unordered_map<FileId, std::uint32_t, FileIdHash> by_id; // entered directories
    std::uint64_t entries = 0;
    std::uint64_t live_dirs = 0;
    std::int64_t scan_started_at = 0;
    bool complete = false;
    bool truncated = false;
    std::uint64_t last_serial = 0;

    // A tree with only the root.
    void reset();

    [[nodiscard]] static std::string_view raw_name(const Dir& d, const Child& c) noexcept {
        return std::string_view(d.pool).substr(c.raw_off, c.raw_len);
    }
    [[nodiscard]] static std::string_view folded_name(const Dir& d, const Child& c) noexcept {
        return std::string_view(d.pool).substr(c.fold_off, c.fold_len);
    }

    // New directory record (child `name` of `parent`, not scanned).
    std::uint32_t new_dir(std::uint32_t parent, std::string_view name);
    // Frees `d` and every directory below it; collects their watch handles; subtracts their
    // children from `entries`. Does not touch the parent's child record.
    void free_subtree(std::uint32_t d, std::vector<WatchHandle>& handles);
    // Index of the child named `name` in `d`, or kNoIndex.
    [[nodiscard]] std::uint32_t find_child(std::uint32_t d, std::string_view name) const noexcept;
    // Absolute path of directory `d`.
    [[nodiscard]] Path path_of(std::uint32_t d, const Path& root) const;
    // Relative path of `d` below `ancestor` ("" when equal).
    [[nodiscard]] std::string relative_path(std::uint32_t d, std::uint32_t ancestor) const;
    // Associates (device, inode) with d (when known) for cycle checks.
    void set_identity(std::uint32_t d, std::uint64_t device, std::uint64_t inode);
};

// Rebuilds d's pool without garbage (offsets of its children change).
void compact_pool(Dir& d, std::string& fold_scratch);

// Appends name and its folded form to `pool`, filling the offsets of `c`. Returns false when the
// name is too long to index.
bool append_name(std::string& pool, Child& c, std::string_view name, std::string& fold_scratch);

// ---------------------------------------------------------------------------------------------
// File codec (navigator/index.hpp "File format (version 1)")
// ---------------------------------------------------------------------------------------------

// Serializes `tree` (only reachable live directories, renumbered breadth-first).
[[nodiscard]] Result<std::string> encode_index(const Tree& tree, const VolumeIdentity& identity,
                                               std::int64_t saved_at_ns);
// Parses and validates; never crashes on hostile input. Errors: InvalidArgument (corrupt or
// wrong version; message says why), NotFound (other volume: message "volume mismatch").
[[nodiscard]] Result<Tree> decode_index(std::string_view bytes, const VolumeIdentity& identity);

// Atomic write: unique temporary file next to `file`, fsync, rename, directory fsync. Creates
// missing parent directories with mode 0700; the file gets 0600.
[[nodiscard]] Result<void> write_file_atomic(const Path& file, std::string_view bytes);
// Reads a whole file of at most kMaxIndexFileBytes. NotFound when it does not exist.
[[nodiscard]] Result<std::string> read_file(const Path& file);

} // namespace navigator::search::detail
