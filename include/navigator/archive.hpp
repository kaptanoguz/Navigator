// navigator/archive.hpp
//
// Purpose: Phase 10 archive browsing and extraction (zip, tar, tar.gz/bz2/xz/zst, 7z, etc.)
//          backed by libarchive (when NAVIGATOR_HAVE_LIBARCHIVE is enabled). Archives are
//          addressed as virtual paths `<archive_file>::<inner_dir>` so the browser can navigate
//          into them like folders, open entries by extracting to a temp file, and extract on
//          drag-out or "Extract Here".
// Owner:   contracts (header); implementation in src/ops (role: ops).
// Thread-safety: every function is reentrant and shares no mutable state. list_archive_directory,
//                extract_entry_to_file, and extract_entries_to_dir perform blocking I/O and must
//                be called off the UI thread (they honour std::stop_token).
// Errors:  navigator::Result (types.hpp). Returns ErrorCode::Unsupported when built without
//          libarchive. No exceptions escape these functions.
#pragma once

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::archive {

inline constexpr std::string_view kArchiveSeparator = "::";

struct ArchivePathRef {
    Path archive_file;      // host filesystem path to the archive file
    std::string inner_path; // normalized relative path inside archive (no leading/trailing '/')
};

// True when Navigator was built with libarchive support.
[[nodiscard]] bool libarchive_available() noexcept;

// True when `name` has a supported archive extension (case-insensitive: .zip, .tar, .tgz,
// .tar.gz, .tbz2, .tar.bz2, .txz, .tar.xz, .tar.zst, .7z, .cbz, .jar).
[[nodiscard]] bool is_archive_name(std::string_view name) noexcept;

// Splits `<archive_file>::<inner_path>` into its components. Returns std::nullopt if `raw` does
// not contain kArchiveSeparator or the archive_file part is empty.
[[nodiscard]] std::optional<ArchivePathRef> parse_archive_path(std::string_view raw);

// Builds `<archive_file>::<inner_path>` (or `<archive_file>::` for the archive root).
[[nodiscard]] Path make_archive_path(const Path& archive_file, std::string_view inner_path = {});

// Lists the direct children of `inner_dir` (empty for archive root) inside `archive_file`.
// Synthesizes implicit directory entries when an archive only stores file paths (e.g. "a/b.txt").
// Errors: Unsupported (no libarchive), NotFound (archive missing or inner_dir not found),
//         IoError (corrupt or unreadable archive), Cancelled.
[[nodiscard]] Result<std::vector<fs::DirEntry>>
list_archive_directory(const Path& archive_file, std::string_view inner_dir,
                       const std::stop_token& stop = {});

// Extracts a single file entry (`inner_path`) from `archive_file` to `dest_file` (creating parent
// directories if needed). Rejects path traversal ("..") and symlinks escaping dest.
// Errors: Unsupported, NotFound, InvalidArgument, IoError, Cancelled.
[[nodiscard]] Result<void> extract_entry_to_file(const Path& archive_file,
                                                 std::string_view inner_path, const Path& dest_file,
                                                 const std::stop_token& stop = {});

// Extracts `inner_paths` (or the entire archive when `inner_paths` is empty) from `archive_file`
// into `dest_dir`, returning the list of created host paths. Guards against Zip Slip (".." or
// absolute entry paths).
// Errors: Unsupported, NotFound, InvalidArgument, IoError, Cancelled.
[[nodiscard]] Result<std::vector<Path>>
extract_entries_to_dir(const Path& archive_file, const std::vector<std::string>& inner_paths,
                       const Path& dest_dir, const std::stop_token& stop = {});

} // namespace navigator::archive
