// navigator/fs.hpp
//
// Purpose: directory listing, metadata, name comparison and sorting. The UI never walks the
//          filesystem with std::filesystem directly; it goes through navigator::fs (and later
//          navigator::IIndex).
// Owner:   contracts (header); implementation in src/core (role: core-fs). Phase 1.
// Thread-safety: every function here is reentrant and shares no mutable state, so any number of
//                threads may call them concurrently. list_directory() and stat() perform blocking
//                I/O: never call them on the UI thread (run them on a worker; the stop_token
//                overload makes a listing cancellable). compare_names(), fold_name(),
//                to_display_name() and sort_entries() are pure CPU work. DirEntry, ListOptions
//                and SortOptions are plain values (see types.hpp for the value-type rule).
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions; allocation
//          failure is reported as ErrorCode::OutOfMemory where a Result is returned.
//
// Names and encoding
//   DirEntry::name holds the file name exactly as the kernel returned it (getdents64 bytes).
//   It is usually UTF-8 but may contain any byte except '/' and NUL. No Unicode normalization is
//   ever applied: an NFC "é" (C3 A9) and an NFD "é" (65 CC 81) are different names and both may
//   exist in one directory. Use to_display_name() to show a name, never to address a file.
//
// Symlink policy (Phase 1 default: do not resolve links at listing time)
//   A symlink is always reported with kind == EntryKind::Symlink. With
//   ListOptions::follow_symlinks == false (default) every metadata field describes the link
//   itself (lstat). The only extra work is one stat() of the target to fill target_kind and
//   dangling, so the UI can draw "link to folder" and folders_first can group it. With
//   follow_symlinks == true the metadata fields (size, mtime, mode, uid, gid, id, readable)
//   describe the target instead; a dangling link keeps its lstat metadata. Neither mode
//   recurses into, canonicalizes or reads (readlink) the target path.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>

namespace navigator::fs {

// Coarse kind. The exact type of an Other entry (fifo, socket, block/char device) is in
// DirEntry::mode & S_IFMT.
enum class EntryKind : std::uint8_t {
    Unknown = 0, // metadata unavailable and the directory entry carried no type (DT_UNKNOWN)
    File = 1,
    Directory = 2,
    Symlink = 3,
    Other = 4,
};

struct DirEntry {
    // File name only (no directory part), raw on-disk bytes, never "." or "..".
    std::string name;
    EntryKind kind = EntryKind::Unknown;

    // Symlink only (Unknown/false otherwise). target_kind: kind of what the link points to,
    // following the whole chain; never Symlink. Unknown when the target cannot be stat'ed.
    // dangling: the target does not exist (stat failed with ENOENT or ENOTDIR). A loop (ELOOP)
    // or EACCES leaves dangling == false and target_kind == Unknown.
    EntryKind target_kind = EntryKind::Unknown;
    bool dangling = false;

    // st_size in bytes for files; for a symlink with follow_symlinks == false, the length of the
    // link text; 0 for directories and Other (filesystem-specific st_size is not exposed).
    std::uint64_t size = 0;
    Timestamp mtime{};      // st_mtim; Timestamp{} when unknown
    std::uint32_t mode = 0; // full st_mode (S_IFMT type bits | permission bits incl. suid/sticky)
    std::uint32_t uid = 0;  // st_uid
    std::uint32_t gid = 0;  // st_gid
    FileId id;              // st_dev/st_ino of whatever the metadata describes

    // Name starts with '.'. (Later phases may add a .hidden file list; this flag will cover it.)
    bool hidden = false;

    // Best-effort "the current process can read this" (for a directory: list it), computed from
    // mode/uid/gid against the effective uid, effective gid and supplementary groups, without an
    // access() call per entry. euid 0 is treated as readable. ACLs, capabilities, LSMs and
    // read-only mounts are ignored, so this is a hint for greying out, not a guarantee.
    bool readable = false;

    // Set when the per-entry metadata call failed (e.g. the entry vanished between readdir and
    // stat, or EACCES on a FUSE mount). The entry is still listed with its name, kind taken from
    // d_type (or Unknown), and all metadata fields at their defaults. A failed target stat of a
    // symlink is NOT a metadata error (see target_kind/dangling).
    std::optional<ErrorCode> metadata_error;
};

// True for names that start with '.'.
[[nodiscard]] constexpr bool is_hidden_name(std::string_view name) noexcept {
    return !name.empty() && name.front() == '.';
}

// Suffix after the last '.', without the dot, as raw bytes ("a.tar.gz" -> "gz"). Empty when there
// is no dot, when the only dot is the leading one (".bashrc" -> ""), or when the name ends with a
// dot ("x." -> ""). Not case-folded.
[[nodiscard]] constexpr std::string_view extension(std::string_view name) noexcept {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return name.substr(dot + 1);
}

// Directory, or symlink whose target is a directory. This is the "folder" test used by
// SortOptions::folders_first and by navigation.
[[nodiscard]] constexpr bool is_directory_like(const DirEntry& entry) noexcept {
    return entry.kind == EntryKind::Directory ||
           (entry.kind == EntryKind::Symlink && entry.target_kind == EntryKind::Directory);
}

struct ListOptions {
    // Include names starting with '.'. Filtering happens before any stat, so hidden entries cost
    // only the readdir when excluded.
    bool show_hidden = false;
    // See "Symlink policy" above. false: metadata of the link itself + one target stat.
    bool follow_symlinks = false;
};

// Lists the immediate children of `dir` (non-recursive). `dir` itself may be a symlink to a
// directory; it is followed. Relative paths resolve against the process working directory.
//
// Order: unspecified (kernel readdir order). Call sort_entries() for a defined order.
//
// Cost: one open + getdents64 sweep, one fstatat(AT_SYMLINK_NOFOLLOW) per listed entry, plus one
// fstatat(follow) per symlink. No per-entry heap allocation other than the name string beyond
// the small-string buffer. Implementations should stat relative to the open directory fd.
// Phase 1 acceptance: 100k entries listed + sorted in a test, timing recorded in docs.
//
// Errors (the call fails only when the directory itself cannot be read; per-entry failures are
// reported in DirEntry::metadata_error and never fail the listing):
//   InvalidArgument  - empty path; `dir` is not a directory (ENOTDIR); ELOOP; ENAMETOOLONG
//   NotFound         - ENOENT
//   PermissionDenied - EACCES / EPERM on open, or a readdir that fails with EACCES
//   OutOfMemory      - ENOMEM or std::bad_alloc
//   IoError          - EIO and any other errno (message includes strerror text and the path)
//   Cancelled        - only from the stop_token overload
[[nodiscard]] Result<std::vector<DirEntry>> list_directory(const Path& dir,
                                                           const ListOptions& options = {});

// Cancellable variant for worker threads. `stop` is checked before the first read and at least
// once every 1024 entries; once a stop is requested the call returns ErrorCode::Cancelled and
// discards partial results. Otherwise identical to the overload above.
[[nodiscard]] Result<std::vector<DirEntry>>
list_directory(const Path& dir, const ListOptions& options, std::stop_token stop);

// Metadata of one path, with the same field semantics as a listed entry.
// follow_symlinks == false: lstat semantics + target classification (as in listing).
// follow_symlinks == true:  as ListOptions::follow_symlinks == true.
// DirEntry::name is the last non-empty component of `path` ("/a/b/" -> "b"; "/" -> "/").
// metadata_error is never set: a failed stat fails the call.
// Errors: InvalidArgument (empty path, ELOOP, ENAMETOOLONG), NotFound (ENOENT, ENOTDIR on a
// prefix), PermissionDenied, OutOfMemory, IoError.
[[nodiscard]] Result<DirEntry> stat(const Path& path, bool follow_symlinks = false);

// ---------------------------------------------------------------------------------------------
// Sort options (used by compare_names and sort_entries)
// ---------------------------------------------------------------------------------------------

enum class SortKey : std::uint8_t {
    Name = 0,      // compare_names
    Size = 1,      // DirEntry::size
    Modified = 2,  // DirEntry::mtime
    Extension = 3, // compare_names(extension(a), extension(b)); the "Type" column
    Count = 4,     // reserved: child count of directories (later phase); currently sorts as Name
};

struct SortOptions {
    SortKey key = SortKey::Name;
    bool descending = false;
    bool folders_first = true;    // is_directory_like() entries form a group placed first
    bool natural = true;          // numeric digit runs, see compare_names
    bool case_insensitive = true; // fold as described above

    friend constexpr bool operator==(const SortOptions&, const SortOptions&) = default;
};

// ---------------------------------------------------------------------------------------------
// Names: folding, comparison, display
// ---------------------------------------------------------------------------------------------
//
// Case folding (locale-independent, identical on every machine and language setting):
//   1. Decode the name as UTF-8. Each byte that is not part of a valid, shortest-form UTF-8
//      sequence (including encoded surrogates) is treated as the code point U+DC00 + byte
//      ("surrogateescape"), so every byte string has exactly one decoding.
//   2. Apply Unicode simple case folding: CaseFolding.txt entries with status C and S (not F,
//      not T), from the Unicode version the implementation embeds (record it in src/core).
//   3. Navigator extension for Turkish: U+0130 (I WITH DOT ABOVE) and U+0131 (DOTLESS I) both
//      fold to U+0069 ('i'). With step 2 ('I' -> 'i') all four of I, i, İ, ı are equivalent, so
//      "İstanbul", "istanbul", "Istanbul" and "ıstanbul" sort together regardless of locale.
//      This is an equivalence for sorting/matching, not Turkish collation (which would order ı
//      before i); locale-aware collation is out of scope for Phase 1.
//   No normalization: NFD "e" + U+0301 folds to the same two code points and compares by them.

// Folded form of `name` as UTF-8 (escaped bytes from step 1 are emitted unchanged, so the result
// is valid UTF-8 iff the input was). Intended for search/filter matching and tests.
[[nodiscard]] std::string fold_name(std::string_view name);

// Name order used by sort_entries(). Only `natural` and `case_insensitive` are read from options.
//
// Compares code point by code point (after folding when case_insensitive); for valid UTF-8 this
// equals byte order of the (folded) strings, and escaped bytes compare as U+DC00 + byte. A name
// that is a proper prefix of the other sorts first.
// When natural: where BOTH names have a maximal run of ASCII digits '0'..'9' at the current
// position, the runs are compared as non-negative integers of unlimited length (leading zeros
// ignored), so "file9" < "file10". Where only one side has a digit, its first digit is compared
// as an ordinary code point, so "file.txt" < "file1.txt" < "fileA.txt" (ASCII order). Runs with
// equal value but different leading zeros ("a01" vs "a1") are equivalent here. Non-ASCII digits
// are ordinary code points.
//
// Returns std::weak_ordering: equivalent names ("README" vs "readme", "a01" vs "a1") compare
// equivalent. sort_entries() breaks such ties by raw bytes (see below).
[[nodiscard]] std::weak_ordering compare_names(std::string_view lhs, std::string_view rhs,
                                               const SortOptions& options) noexcept;

// Converts raw name bytes to valid UTF-8 for display: valid sequences are copied, every invalid
// byte becomes U+FFFD. Lossy: never use the result as a path.
[[nodiscard]] std::string to_display_name(std::string_view name);

// ---------------------------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------------------------

// Sorts in place. Comparison chain, all evaluated left to right:
//   1. folders_first: is_directory_like() entries before the rest. Not affected by descending.
//   2. key (Name, Size, Modified, Extension, Count-as-Name).
//   3. compare_names(name)   (skipped when key is Name/Count: already compared)
//   4. raw byte order of name (std::string_view <=>), which makes the order total for distinct
//      names, so the result is deterministic and independent of input order.
// descending reverses steps 2-4 together (the result within each group is the exact reverse of
// the ascending order). Entries with metadata_error sort with their default field values (size
// 0, mtime epoch). Stable for byte-identical names. O(n log n) comparisons; no allocation per
// comparison (implementations must not call fold_name() inside the comparator).
void sort_entries(Span<DirEntry> entries, const SortOptions& options);

} // namespace navigator::fs
