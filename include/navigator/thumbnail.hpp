// navigator/thumbnail.hpp
//
// Purpose: Phase 6 Freedesktop thumbnails (Thumbnail Managing Standard 0.9): cache keys and
//          paths (file URI, MD5 name, size classes), validated read, atomic write, failure
//          markers, the generation policy; and system thumbnailers (*.thumbnailer files in the
//          XDG data dirs: parse, choose per MIME, expand Exec, run through an injected
//          platform::IProcessRunner). Sharing the standard cache means thumbnails made by
//          Nautilus, Nemo, Thunar, ... are reused and ours are reused by them (a Navigator
//          extra, docs/features.md).
// Owner:   contracts (header); implementation in src/preview (role: preview), tests in
//          tests/preview. Paths are platform-owned values: the caller passes the cache root
//          (platform::thumbnail_cache_dir(), or the AppConfig override) and the data dirs
//          (platform::data_dirs()); src/preview never reads the environment for them.
// Thread-safety: the pure functions (marked Pure) may be called from any thread, including the
//                UI thread. The file functions block on I/O and run on preview workers. All
//                functions are reentrant; concurrent writers of the same thumbnail are safe
//                (each writes its own temporary file, the last rename wins; other programs do
//                the same).
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
//
// Cache layout (spec):
//   <root>/normal/<md5>.png     at most 128 x 128
//   <root>/large/<md5>.png      at most 256 x 256
//   <root>/x-large/<md5>.png    at most 512 x 512
//   <root>/xx-large/<md5>.png   at most 1024 x 1024
//   <root>/fail/navigator-<rev>/<md5>.png   failure marker (ours only; other programs' fail
//                                           dirs are never read: their decoders differ)
//   <md5> = lowercase hex MD5 of the file URI (file_uri()).
//   Every PNG carries tEXt chunks: Thumb::URI (required), Thumb::MTime (required, integer
//   seconds of st_mtime), Thumb::Size (file size in bytes), Thumb::Mimetype,
//   Thumb::Image::Width / Thumb::Image::Height (original pixel size, display orientation),
//   Thumb::Movie::Length (seconds, videos), Software ("Navigator <version>").
//   Thumbnails are stored in display orientation (EXIF applied), straight alpha, never larger
//   than the original (no upscaling), the longer side equal to the class size otherwise.
//
// Symlinks: the URI is that of the path as browsed (the link), lexically normal, symlinks not
// resolved, matching what GIO-based file managers store. Thumb::MTime / Thumb::Size are those of
// the TARGET (stat following links), so a changed target invalidates the thumbnail.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/platform.hpp>
#include <navigator/preview.hpp>
#include <navigator/types.hpp>

namespace navigator::thumbnail {

// =============================================================================================
// Size classes and paths
// =============================================================================================

enum class ThumbnailSize : std::uint16_t {
    Normal = 128,
    Large = 256,
    XLarge = 512,
    XXLarge = 1024,
};
inline constexpr std::size_t kThumbnailSizeCount = 4;

[[nodiscard]] constexpr std::uint32_t pixels(ThumbnailSize size) noexcept {
    return static_cast<std::uint32_t>(size);
}

// Directory name of the class: "normal", "large", "x-large", "xx-large". Pure.
[[nodiscard]] constexpr std::string_view dir_name(ThumbnailSize size) noexcept {
    switch (size) {
    case ThumbnailSize::Normal: return "normal";
    case ThumbnailSize::Large: return "large";
    case ThumbnailSize::XLarge: return "x-large";
    case ThumbnailSize::XXLarge: return "xx-large";
    }
    return "normal";
}

// Smallest class whose pixel size is >= device_px (the larger side of the ui's icon box in
// DEVICE pixels: a 48 dp icon at scale 2 needs Large), XXLarge above 1024. Pure.
[[nodiscard]] constexpr ThumbnailSize size_for(std::uint32_t device_px) noexcept {
    if (device_px <= pixels(ThumbnailSize::Normal)) {
        return ThumbnailSize::Normal;
    }
    if (device_px <= pixels(ThumbnailSize::Large)) {
        return ThumbnailSize::Large;
    }
    if (device_px <= pixels(ThumbnailSize::XLarge)) {
        return ThumbnailSize::XLarge;
    }
    return ThumbnailSize::XXLarge;
}

inline constexpr std::string_view kCacheDirName = "thumbnails"; // under $XDG_CACHE_HOME
inline constexpr std::string_view kFailDirName = "fail";
// Our failure-marker subdirectory. The revision is bumped whenever the decoders gain formats or
// fixes, so that old failures are retried by the new version.
inline constexpr std::string_view kFailAppDirName = "navigator-1";
inline constexpr std::string_view kSoftware = "Navigator";

// <xdg_cache_home>/thumbnails. Pure (the caller supplies platform::user_dir(Cache)).
[[nodiscard]] inline Path cache_root_for(const Path& xdg_cache_home) {
    return xdg_cache_home / kCacheDirName;
}

// "file://" + the percent-encoded lexically normal absolute path, exactly as GLib's
// g_filename_to_uri() (and so g_file_get_uri() for local files) writes it: bytes in
// [A-Za-z0-9] and - . _ ~ ! $ & ' ( ) * + , / : = @ are kept; every other byte (space, " # % ;
// < > ? [ \ ] ^ ` { | }, controls, and every byte >= 0x80, so UTF-8 names are encoded byte by
// byte) becomes %XX with UPPERCASE hex. Example: "/home/oğuz/a b.jpg" ->
// "file:///home/o%C4%9Fuz/a%20b.jpg". A different encoding would miss every thumbnail other
// programs made (tests/preview checks reference strings produced by GLib). Pure.
// Errors: InvalidArgument (empty or relative path), OutOfMemory.
[[nodiscard]] Result<std::string> file_uri(const Path& absolute_path);

// Lowercase hex MD5 (RFC 1321) of the bytes, 32 characters. Pure.
[[nodiscard]] std::string md5_hex(std::string_view bytes);

// md5_hex(uri) + ".png". Pure.
[[nodiscard]] std::string thumbnail_file_name(std::string_view uri);

// <cache_root>/<dir_name(size)>/<thumbnail_file_name(uri)>. Pure.
[[nodiscard]] Path thumbnail_path(const Path& cache_root, ThumbnailSize size, std::string_view uri);

// <cache_root>/fail/navigator-<rev>/<thumbnail_file_name(uri)>. Pure.
[[nodiscard]] Path failure_path(const Path& cache_root, std::string_view uri);

// True when `path` is `cache_root` or below it, compared component-wise on lexically normal
// absolute paths (no I/O; a symlink into the cache is not detected, which only costs a
// thumbnail of a thumbnail). Such files are never thumbnailed. Pure.
[[nodiscard]] bool is_inside_cache(const Path& path, const Path& cache_root);

// st_mtime as the spec's integer seconds (floor of the Timestamp). Pure.
[[nodiscard]] constexpr std::int64_t mtime_seconds(Timestamp mtime) noexcept {
    return std::chrono::floor<std::chrono::seconds>(mtime).time_since_epoch().count();
}

// =============================================================================================
// Reading and writing
// =============================================================================================

// What a thumbnail is validated against.
struct SourceInfo {
    Path path;         // absolute path as browsed
    std::string uri;   // file_uri(path)
    Timestamp mtime{}; // target's st_mtime (only whole seconds are compared)
    std::uint64_t size = 0;
    std::string mime; // written as Thumb::Mimetype; may be empty
};

enum class LookupStatus : std::uint8_t {
    Valid = 0,   // URI and MTime match (and Size, when the file has Thumb::Size)
    Stale = 1,   // a thumbnail for this URI exists but MTime or Size differ: regenerate
    Missing = 2, // no file
    Invalid = 3, // not a PNG, corrupt, dimensions 0 or > 4096, or no / wrong Thumb::URI:
                 // regenerate and overwrite
};

struct ThumbnailRead {
    LookupStatus status = LookupStatus::Missing;
    ThumbnailSize size_class = ThumbnailSize::Normal; // class of `file`
    Path file;                   // the file that was examined (the first one found)
    preview::ImageBuffer image;  // Valid only: premultiplied RGBA8, as stored (display orientation)
    preview::PixelSize original; // Thumb::Image::Width/Height when present, else {0, 0}
    std::optional<std::int64_t> movie_length_s; // Thumb::Movie::Length when present
};

// Reads and validates one thumbnail file. Only the PNG signature, IHDR and the tEXt/zTXt/iTXt
// chunks before IDAT are parsed until the keys are validated; pixels are decoded only for a
// Valid file (bounded: at most 4096 x 4096). ENOENT is status Missing, not an error.
// Errors: PermissionDenied, IoError, Cancelled, OutOfMemory.
[[nodiscard]] Result<ThumbnailRead> read_thumbnail(const Path& file, const SourceInfo& expected,
                                                   std::stop_token stop);

// Looks for `wanted`, then each larger class, in order; returns the first Valid one (callers
// downscale). If none is valid: Stale if any examined file was stale, else Invalid if any was
// invalid, else Missing; `file` then names the `wanted` class path. Errors: as read_thumbnail.
[[nodiscard]] Result<ThumbnailRead> lookup_thumbnail(const Path& cache_root, ThumbnailSize wanted,
                                                     const SourceInfo& expected,
                                                     std::stop_token stop);

struct WriteOptions {
    preview::PixelSize original;                // Thumb::Image::Width/Height; {0,0}: omitted
    std::optional<std::int64_t> movie_length_s; // Thumb::Movie::Length; videos
};

// Writes <cache_root>/<class>/<md5>.png atomically: missing directories are created with mode
// 0700 (existing ones are not touched: the cache is shared), the PNG (straight alpha, with every
// tEXt key listed at the top) goes to a unique temporary file "<md5>.png.XXXXXX" in the same
// directory created with mode 0600, then rename()d over the final name; on any failure the
// temporary file is removed. No fsync (a lost thumbnail is regenerated). Returns the final path.
// Errors: InvalidArgument (!image.valid(), a side larger than pixels(size), empty uri, or
// is_inside_cache(source.path)), PermissionDenied, IoError (includes ENOSPC/EROFS), OutOfMemory.
Result<Path> write_thumbnail(const Path& cache_root, ThumbnailSize size, const SourceInfo& source,
                             const preview::ImageBuffer& image, const WriteOptions& options);

// Writes the failure marker (a 1 x 1 transparent PNG with Thumb::URI and Thumb::MTime) to
// failure_path(), atomically as write_thumbnail. Written when a decoder reported the file
// corrupt (InvalidArgument) or an external thumbnailer failed / timed out / wrote no image;
// never for Cancelled, OutOfMemory or Unsupported (no provider).
// Errors: as write_thumbnail.
Result<void> write_failure(const Path& cache_root, const SourceInfo& source);

// True when our failure marker exists and its Thumb::URI and Thumb::MTime match (a changed
// file is retried). A corrupt marker counts as absent. Errors: PermissionDenied, IoError.
[[nodiscard]] Result<bool> has_failure(const Path& cache_root, const SourceInfo& source);

// =============================================================================================
// Generation policy
// =============================================================================================

struct Policy {
    // Agreed Phase 6 defaults: generate new thumbnails only for local fixed volumes. Files on
    // network and removable volumes still use thumbnails that already exist in the cache
    // (reading costs one local file; the source is never opened), and the Inspector always
    // decodes the one inspected file. Phase 9 settings may turn these on; a camera card user
    // is the expected reason to enable `removable`.
    bool generate_on_network = false;
    bool generate_on_removable = false;

    friend constexpr bool operator==(const Policy&, const Policy&) = default;
};

enum class Decision : std::uint8_t {
    Generate = 0, // read the cache; on a miss produce and write
    ReadOnly = 1, // read the cache; on a miss PlaceholderPreview{NotGenerated}
    Skip = 2,     // no thumbnail at all (no cache read)
};

// Pure. In order:
//   - path empty or relative, or is_inside_cache(path, cache_root)            -> Skip
//   - entry is not a regular file or a symlink whose target_kind is File      -> Skip
//     (folders, devices, fifos, sockets, dangling links)
//   - location.network && !policy.generate_on_network                         -> ReadOnly
//   - location.removable && !policy.generate_on_removable                     -> ReadOnly
//   - otherwise                                                               -> Generate
// An empty cache_root (disk cache disabled) is handled by the service, not here.
[[nodiscard]] Decision decide(const Path& path, const fs::DirEntry& entry,
                              const preview::SourceLocation& location, const Path& cache_root,
                              const Policy& policy);

// =============================================================================================
// System thumbnailers (*.thumbnailer)
// =============================================================================================
//
// File format (desktop-entry syntax, as gnome-desktop / tumbler read it):
//   [Thumbnailer Entry]
//   TryExec=ffmpegthumbnailer
//   Exec=ffmpegthumbnailer -i %i -o %o -s %s -f
//   MimeType=video/jp2;video/mp4;video/x-matroska;...
// Parsing: UTF-8 text up to 64 KiB; lines "Key=Value" (spaces around '=' trimmed); '#' comment
// lines and blank lines ignored; only the [Thumbnailer Entry] group is read (others skipped);
// localized keys ("Key[xx]") ignored; value escapes \s \n \t \r \\ ; MimeType is ';'-separated
// ("\;" escapes a ';'), empty items dropped, ASCII-lowercased, duplicates removed. Exec and
// MimeType are required; TryExec optional.

struct Thumbnailer {
    std::string id;       // file name without ".thumbnailer" (shadowing key)
    Path file;            // where it was read
    std::string try_exec; // may be empty
    std::string exec;     // raw Exec value (validated by split_exec)
    std::vector<std::string> mime_types;
};

// Pure. Errors: InvalidArgument (no [Thumbnailer Entry] group, missing or empty Exec or
// MimeType, Exec rejected by split_exec, more than 64 KiB), OutOfMemory.
[[nodiscard]] Result<Thumbnailer> parse_thumbnailer(std::string_view text, std::string_view id,
                                                    const Path& file);

// Splits Exec into words with the POSIX-shell subset that GLib's g_shell_parse_argv accepts:
// blanks separate words; '...' is literal; inside "..." backslash escapes only \ " $ ` and
// newline; outside quotes a backslash escapes the next character. Nothing else is special:
// no variables, globs, pipes, redirections or command substitution are interpreted (such
// characters stay literal bytes of the word), and no shell is ever run. Field codes are checked
// but not expanded: %i %o %s %u %% are allowed anywhere in a word; any other % sequence is an
// error. Pure. Errors: InvalidArgument (unterminated quote, trailing backslash, unknown field
// code, no words), OutOfMemory.
[[nodiscard]] Result<std::vector<std::string>> split_exec(std::string_view exec);

struct ExecInputs {
    std::string_view input_path;  // %i: absolute path of the source (as browsed)
    std::string_view input_uri;   // %u: file_uri() of it
    std::string_view output_path; // %o: absolute path of the PNG to write (temp dir)
    std::uint32_t size = 0;       // %s: pixels(class), decimal
};

// split_exec() then substitutes field codes inside each word (%% -> %). A substituted value is
// never re-split, so spaces or quotes in a file name cannot add arguments. Pure.
// Errors: as split_exec; InvalidArgument when an input used by the Exec line is empty.
[[nodiscard]] Result<std::vector<std::string>> expand_exec(std::string_view exec,
                                                           const ExecInputs& inputs);

// data_dir / "thumbnailers" for each data dir, same order (platform::data_dirs()). Pure.
[[nodiscard]] std::vector<Path> thumbnailer_dirs(Span<const Path> data_dirs);

// True when the program can be run (production: platform::find_program succeeds).
using ProgramExists = std::function<bool(std::string_view program)>;

// Reads every "*.thumbnailer" regular file directly in each dir (not recursive; missing or
// unreadable dirs and invalid files are skipped, never fatal). An id already found in an
// earlier dir shadows later ones (XDG precedence). An entry is dropped when its TryExec is set
// and !exists(TryExec), or when !exists(first word of Exec). Result order: dir order, then id
// byte order within a dir. Blocking (directory listing, small reads).
// Errors: OutOfMemory, Cancelled.
[[nodiscard]] Result<std::vector<Thumbnailer>>
load_thumbnailers(Span<const Path> dirs, const ProgramExists& exists, std::stop_token stop);

// First thumbnailer (in load order) listing `mime` (ASCII case-insensitive exact match; no
// wildcards, no MIME subclassing in Phase 6). nullptr when none. Pure.
[[nodiscard]] const Thumbnailer* choose_thumbnailer(Span<const Thumbnailer> thumbnailers,
                                                    std::string_view mime) noexcept;

// Defaults for running one thumbnailer.
inline constexpr std::chrono::milliseconds kDefaultThumbnailerTimeout{15'000};
inline constexpr std::uint64_t kDefaultThumbnailerOutputBytes = std::uint64_t{32} * 1024U * 1024U;
inline constexpr std::uint64_t kDefaultThumbnailerAddressSpace =
    std::uint64_t{2} * 1024U * 1024U * 1024U;
inline constexpr std::size_t kDefaultThumbnailerConcurrency = 2;

struct ThumbnailerLimits {
    std::chrono::milliseconds timeout = kDefaultThumbnailerTimeout;
    std::uint64_t max_output_bytes = kDefaultThumbnailerOutputBytes; // RLIMIT_FSIZE + size check
    std::uint64_t address_space_bytes = kDefaultThumbnailerAddressSpace;
    std::chrono::seconds cpu_time{30};
    bool sandbox = true; // ProcessRequest::sandbox (bubblewrap when available)
    // At most this many thumbnailer processes run at once across all workers (the service
    // enforces it; further requests wait in the queue, cancellable).
    std::size_t max_concurrent = kDefaultThumbnailerConcurrency;

    friend constexpr bool operator==(const ThumbnailerLimits&, const ThumbnailerLimits&) = default;
};

// Runs `thumbnailer` for `source` at `size`:
//   1. creates a private temporary directory (mkdtemp, mode 0700) under $TMPDIR or /tmp;
//   2. expand_exec with %o = <tmp>/out.png;
//   3. runner.run({argv, working_dir = <tmp>, the limits, sandbox, read_only_paths =
//      {source.path}, writable_paths = {<tmp>}}, stop);
//   4. requires succeeded(), then out.png to be a regular file of 1..max_output_bytes bytes;
//      decodes it (PNG, or any format sniff_image_format recognizes and a built-in decoder
//      supports) with the kMaxFullDecodePixels bound and fits it into pixels(size);
//   5. removes the temporary directory in every case.
// The thumbnailer's own tEXt chunks are ignored: write_thumbnail() writes ours.
// Errors: InvalidArgument (Exec expansion failed, or the output is not a decodable image),
// NotFound (program vanished), Cancelled, IoError (non-zero exit, signal, timeout, no or
// oversized output; the message says which), PlatformError, OutOfMemory.
[[nodiscard]] Result<preview::ImageBuffer>
run_thumbnailer(const Thumbnailer& thumbnailer, const SourceInfo& source, ThumbnailSize size,
                platform::IProcessRunner& runner, const ThumbnailerLimits& limits,
                std::stop_token stop);

} // namespace navigator::thumbnail
