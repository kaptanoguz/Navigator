// navigator/preview.hpp
//
// Purpose: Phase 6 preview vocabulary (Inspector and list thumbnails): decoded pixel buffers,
//          image / EXIF / video / text / folder results, the request and its cache key, the
//          IPreviewProvider plug-in point, the ProviderRegistry, and pure helpers (EXIF
//          orientation, fit size, JPEG DCT scale choice, format sniffing, text decoding).
//          The Freedesktop thumbnail cache and external thumbnailers are in thumbnail.hpp; the
//          worker pool, memory cache and the built-in providers in preview_service.hpp.
// Owner:   contracts (header); implementation in src/preview (role: preview), tests in
//          tests/preview. src/preview links navigator_core (fs listing for folder previews) and
//          its decoder libraries PRIVATE; it never calls src/platform (process running and XDG
//          paths come in as injected objects and values) and never sees SDL.
// Thread-safety: every type here is a plain value (types.hpp rule). The free functions are
//                pure CPU work, reentrant, and callable from any thread (decode_text() and
//                apply_exif_orientation() are O(bytes): workers only for large inputs).
//                IPreviewProvider: see the class comment (called concurrently on worker
//                threads). ProviderRegistry: add() during setup on one thread; afterwards its
//                const members may be called from any number of threads concurrently.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions; allocation
//          failure is reported as ErrorCode::OutOfMemory. Error meanings for providers:
//            Unsupported      this provider cannot show this file (the registry tries the next)
//            InvalidArgument  the file's content is corrupt or violates a limit below (no
//                             fallback; the ui shows "cannot preview this file")
//            NotFound / PermissionDenied / IoError   reading the file failed
//            Cancelled        the request's stop token fired
//            OutOfMemory      allocation failed or the decode would exceed its memory budget
//
// Untrusted input. Every file is hostile until decoded: providers check declared dimensions
// against the limits below BEFORE allocating, decode with bounded memory (downscaling while
// decoding where the format allows it), never trust sizes or offsets inside the file, check the
// stop token at least every 64 rows / 1 MiB of input, and turn every decoder error (libjpeg
// longjmp, libpng error callback, ...) into a Result. A corrupt or truncated file is an error
// or a partial image, never a crash, hang or unbounded allocation (tests/preview fuzz-style
// fixtures: truncated, bit-flipped, huge declared size).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::preview {

// =============================================================================================
// Sizes and limits
// =============================================================================================

// Pixel dimensions. Device pixels for targets (the ui multiplies logical sizes by the display
// scale), image pixels for sources.
struct PixelSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] constexpr std::uint64_t pixels() const noexcept {
        return std::uint64_t{width} * std::uint64_t{height};
    }

    friend constexpr bool operator==(const PixelSize&, const PixelSize&) = default;
};

// A declared width or height above this is rejected with InvalidArgument before any
// allocation (larger than any camera or scanner output; PNG would allow 2^31).
inline constexpr std::uint32_t kMaxSourceDimension = 65535;
// Declared width * height above this is rejected with InvalidArgument (1 GP; a 100 MP photo is
// fine). Formats that can downscale while decoding (JPEG DCT scaling, streaming PNG/WebP/BMP
// row reduction) decode any source up to this limit into a small buffer.
inline constexpr std::uint64_t kMaxSourcePixels = std::uint64_t{1} << 30U;
// Formats that must decode the whole image before scaling (HEIF/AVIF through libheif, animated
// GIF frames, SVG rasterization) refuse sources above this with OutOfMemory, after trying an
// embedded thumbnail (HEIF/EXIF) if the request is a Thumbnail.
inline constexpr std::uint64_t kMaxFullDecodePixels = std::uint64_t{64} * 1024U * 1024U;
// Largest ImageBuffer a provider returns (32 MP = 128 MiB of RGBA8). An Inspector request at
// full resolution ({0, 0} target) of a larger image gets the largest fit_size() under this
// limit; ImagePreview::info still reports the true size, so the ui labels 1:1 as "scaled".
inline constexpr std::uint64_t kMaxOutputPixels = std::uint64_t{32} * 1024U * 1024U;
// Peak memory one decode may use (output + working buffers), default for providers.
inline constexpr std::uint64_t kDefaultDecodeMemoryBytes = std::uint64_t{256} * 1024U * 1024U;

// =============================================================================================
// Decoded pixels
// =============================================================================================
//
// The one pixel format crossing module boundaries: 8-bit RGBA, PREMULTIPLIED alpha, bytes in
// memory R, G, B, A (SDL calls this byte order SDL_PIXELFORMAT_RGBA32; the ui uploads with the
// premultiplied blend mode). Rows run top to bottom, `stride` bytes apart. Premultiplied so that
// downscaling, the thumbnail fade and blending over the checkerboard need no fix-ups at edges;
// writers of straight-alpha formats (Freedesktop PNG thumbnails) un-premultiply. Pixels are in
// DISPLAY orientation: EXIF orientation has been applied (see ImageInfo). Colour: sRGB; an
// embedded ICC profile is reported in ImageInfo but not applied in Phase 6.
struct ImageBuffer {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;         // bytes per row: >= width * 4, a multiple of 4
    std::vector<std::uint8_t> pixels; // stride * height bytes
    bool opaque = false;              // hint: every alpha is 255 (false is always safe)

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
    [[nodiscard]] PixelSize size() const noexcept { return {width, height}; }
    // Structural check: non-empty, stride >= width * 4, stride % 4 == 0, pixels.size() ==
    // stride * height. Every buffer a provider returns is valid().
    [[nodiscard]] bool valid() const noexcept {
        return !empty() && stride % 4U == 0U &&
               std::uint64_t{stride} >= std::uint64_t{width} * 4U &&
               pixels.size() == std::uint64_t{stride} * std::uint64_t{height};
    }
};

// Allocates a transparent (all zero) buffer with stride = width * 4.
// Errors: InvalidArgument (empty size, or size.pixels() > kMaxOutputPixels), OutOfMemory.
[[nodiscard]] Result<ImageBuffer> make_image(PixelSize size);

// =============================================================================================
// EXIF
// =============================================================================================

// EXIF tag 0x0112. The comment says how the stored image must be transformed for display.
enum class ExifOrientation : std::uint8_t {
    Normal = 1,         // as stored
    FlipHorizontal = 2, // mirror left-right
    Rotate180 = 3,      // rotate 180
    FlipVertical = 4,   // mirror top-bottom
    Transpose = 5,      // mirror along the top-left / bottom-right diagonal
    Rotate90 = 6,       // rotate 90 degrees clockwise (portrait photo from most cameras)
    Transverse = 7,     // mirror along the top-right / bottom-left diagonal
    Rotate270 = 8,      // rotate 90 degrees counter-clockwise
};

// Values outside 1..8 (absent tag, 0, garbage) mean Normal.
[[nodiscard]] constexpr ExifOrientation orientation_from_tag(std::uint32_t tag) noexcept {
    return tag >= 1U && tag <= 8U ? static_cast<ExifOrientation>(tag) : ExifOrientation::Normal;
}
// True for 5..8: display width/height are the stored height/width.
[[nodiscard]] constexpr bool swaps_axes(ExifOrientation o) noexcept {
    return static_cast<std::uint8_t>(o) >= 5U;
}
[[nodiscard]] constexpr PixelSize oriented_size(PixelSize stored, ExifOrientation o) noexcept {
    return swaps_axes(o) ? PixelSize{stored.height, stored.width} : stored;
}

struct Rational {
    std::uint32_t num = 0;
    std::uint32_t den = 0; // 0: unknown

    [[nodiscard]] constexpr bool known() const noexcept { return den != 0; }
    friend constexpr bool operator==(const Rational&, const Rational&) = default;
};

// EXIF "YYYY:MM:DD HH:MM:SS" (local time of the camera) plus the optional OffsetTime* tag.
struct ExifDateTime {
    std::uint16_t year = 0; // 0: unknown (the whole value is unknown)
    std::uint8_t month = 0; // 1..12
    std::uint8_t day = 0;   // 1..31
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
    std::optional<std::int16_t> utc_offset_minutes; // from OffsetTimeOriginal ("+03:00" -> 180)

    [[nodiscard]] constexpr bool known() const noexcept { return year != 0; }
    friend constexpr bool operator==(const ExifDateTime&, const ExifDateTime&) = default;
};

// What the Inspector shows (docs/review/phase-5.md "EXIF and metadata"). Strings are trimmed,
// NUL padding removed, invalid UTF-8 replaced by U+FFFD, at most 256 bytes; empty = absent.
// Parsed from JPEG APP1, PNG eXIf, WebP EXIF, HEIF Exif items (libexif or an in-tree TIFF IFD
// reader, implementation's choice; offsets bounds-checked, IFD loops and depth > 4 rejected).
struct ExifInfo {
    ExifOrientation orientation = ExifOrientation::Normal;
    std::string make;       // 0x010F
    std::string model;      // 0x0110
    std::string lens_model; // 0xA434
    std::string software;   // 0x0131
    // DateTimeOriginal (0x9003), else DateTimeDigitized (0x9004), else DateTime (0x0132).
    ExifDateTime taken;
    std::optional<Rational> exposure_time;   // seconds, 0x829A (e.g. 1/250)
    std::optional<double> f_number;          // 0x829D
    std::optional<std::uint32_t> iso;        // 0x8827 (PhotographicSensitivity)
    std::optional<double> focal_length_mm;   // 0x920A
    std::optional<std::uint32_t> focal_35mm; // 0xA405
    std::optional<bool> flash_fired;         // bit 0 of 0x9209
    bool has_gps = false;                    // a GPS IFD exists
    std::optional<double> gps_latitude;      // decimal degrees, south negative
    std::optional<double> gps_longitude;     // decimal degrees, west negative
    std::optional<double> gps_altitude_m;    // below sea level negative
    // Coordinates are shown as numbers only: no map, no network (docs/review/phase-5.md).

    friend bool operator==(const ExifInfo&, const ExifInfo&) = default;
};

// =============================================================================================
// Image formats and info
// =============================================================================================

enum class ImageFormat : std::uint8_t {
    Unknown = 0,
    Jpeg,
    Png,
    WebP,
    Gif,
    Bmp,
    Heif, // HEIC / HEIF (libheif, optional build)
    Avif, // AVIF (libheif with an AV1 decoder, optional build)
    Tiff, // sniffed only; no Phase 6 decoder (external thumbnailer may cover it)
    Svg,  // librsvg, optional build; otherwise "coming soon"
    Ico,  // sniffed only
};

// Stable lowercase name: "unknown", "jpeg", "png", "webp", "gif", "bmp", "heif", "avif", "tiff",
// "svg", "ico".
[[nodiscard]] std::string_view to_string(ImageFormat format) noexcept;

// Magic-number detection from the first bytes of a file (pass at least 32 bytes when the file
// has them; fewer bytes simply match less). Pure, no allocation.
//   FF D8 FF                         Jpeg
//   89 50 4E 47 0D 0A 1A 0A          Png
//   "RIFF" ???? "WEBP"               WebP
//   "GIF87a" | "GIF89a"              Gif
//   "BM"                             Bmp   (plus a plausible header size at offset 14)
//   ???? "ftyp" + brand heic heix hevc hevx heim heis mif1 msf1  Heif
//   ???? "ftyp" + brand avif avis    Avif  (checked before Heif: AVIF files often list mif1)
//   "II*\0" | "MM\0*"                Tiff
//   00 00 01 00                      Ico
//   optional UTF-8 BOM / whitespace, then "<svg" or "<?xml" followed by "<svg" within the
//   given bytes                      Svg
// Providers trust the sniffed format over the extension (a ".jpg" that is a PNG decodes as PNG).
[[nodiscard]] ImageFormat sniff_image_format(Span<const std::uint8_t> head) noexcept;

struct ImageInfo {
    ImageFormat format = ImageFormat::Unknown;
    PixelSize stored;  // as encoded in the file (before orientation)
    PixelSize display; // oriented_size(stored, orientation): what the ui lays out
    ExifOrientation orientation = ExifOrientation::Normal; // reported; already APPLIED to pixels
    std::uint8_t bits_per_channel = 8;                     // as encoded (16-bit PNG -> 16)
    bool has_alpha = false;
    bool animated = false;         // GIF / WebP / AVIF sequence; the buffer is the first frame
    std::uint32_t frame_count = 1; // 0: unknown (not counted to keep the decode bounded)
    bool has_icc_profile = false;
    std::optional<ExifInfo> exif; // absent when the file has no (parsable) EXIF block

    friend bool operator==(const ImageInfo&, const ImageInfo&) = default;
};

// =============================================================================================
// Video (a Navigator extra: docs/features.md "Navigator extras")
// =============================================================================================

enum class MetadataSource : std::uint8_t {
    None = 0, // no metadata (built without FFmpeg); the poster may still come from a thumbnailer
    FFmpeg = 1,
};

struct VideoInfo {
    MetadataSource source = MetadataSource::None;
    std::string container;      // FFmpeg demuxer short name, e.g. "matroska,webm", "mov,mp4,..."
    std::string container_long; // human name, e.g. "Matroska / WebM" (untranslated)
    std::optional<std::int64_t> duration_ms;
    PixelSize frame;                   // display size: sample aspect ratio and rotation applied
    std::int16_t rotation_degrees = 0; // 0, 90, 180, 270 from the display matrix (clockwise)
    Rational frame_rate;               // average frame rate (e.g. 30000/1001); den 0: unknown
    std::string video_codec;           // FFmpeg codec short name: "h264", "hevc", "av1", "vp9", ...
    std::uint64_t bit_rate = 0;        // bits per second of the container; 0: unknown
    std::string audio_codec;           // first audio stream: "aac", "opus", ...; empty: none
    std::uint32_t audio_channels = 0;
    std::uint32_t audio_sample_rate = 0;
    std::uint32_t video_streams = 0;
    std::uint32_t audio_streams = 0;
    std::uint32_t subtitle_streams = 0;

    friend bool operator==(const VideoInfo&, const VideoInfo&) = default;
};

// =============================================================================================
// Text
// =============================================================================================
//
// Long lines must never cost frame time (File Pilot reportedly dropped FPS on long lines). The
// decoder therefore splits every logical line into display ROWS of at most max_row_bytes bytes
// (never inside a UTF-8 sequence), and the ui lays out and shapes only the rows that are
// visible: per-frame cost is bounded by visible_rows * max_row_bytes, however long a line is
// (a 4 MiB minified file is 4096 rows of 1 KiB). Wrapping inside a row is the ui's choice.

inline constexpr std::uint64_t kDefaultTextPreviewBytes = std::uint64_t{4} * 1024U * 1024U;
inline constexpr std::uint32_t kDefaultTextRowBytes = 1024;
inline constexpr std::size_t kDefaultTextMaxRows = 500'000;
// Bytes looked at for the binary test (looks_binary).
inline constexpr std::size_t kBinarySniffBytes = 8192;

enum class TextEncoding : std::uint8_t {
    Utf8 = 0,    // no BOM; also the fallback for anything else (invalid bytes -> U+FFFD)
    Utf8Bom = 1, // EF BB BF (stripped)
    Utf16Le = 2, // FF FE (converted to UTF-8; unpaired surrogates -> U+FFFD)
    Utf16Be = 3, // FE FF
};

struct TextOptions {
    std::uint64_t max_bytes = kDefaultTextPreviewBytes; // "first N MB" cap (bytes of the file)
    std::uint32_t max_row_bytes = kDefaultTextRowBytes; // >= 64
    std::size_t max_rows = kDefaultTextMaxRows;

    friend constexpr bool operator==(const TextOptions&, const TextOptions&) = default;
};

// One display row: text.substr(offset, length), without the line terminator. `line` is the
// 0-based logical line; consecutive rows with the same line are continuations of one long line.
struct TextRow {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t line = 0;

    friend constexpr bool operator==(const TextRow&, const TextRow&) = default;
};

struct TextPreview {
    std::string text; // valid UTF-8 (converted / repaired), terminators kept, BOM stripped
    std::vector<TextRow> rows;
    TextEncoding encoding = TextEncoding::Utf8;
    std::uint64_t file_size = 0;          // as given to decode_text
    std::uint64_t bytes_read = 0;         // input bytes consumed (<= options.max_bytes)
    std::uint32_t line_count = 0;         // logical lines in `rows`
    std::uint64_t longest_line_bytes = 0; // within the decoded part
    std::uint64_t replaced_sequences = 0; // invalid sequences replaced by U+FFFD
    // bytes_read < file_size, or max_rows reached. The ui shows "Showing the first N MB".
    bool truncated = false;

    [[nodiscard]] std::string_view row_text(std::size_t row) const noexcept {
        if (row >= rows.size()) {
            return {};
        }
        return std::string_view(text).substr(rows[row].offset, rows[row].length);
    }
};

// True when `head` (the first kBinarySniffBytes of a file) has a NUL byte and does not start with
// a UTF-16 BOM. Pure.
[[nodiscard]] bool looks_binary(Span<const std::uint8_t> head) noexcept;

// Decodes the first min(bytes.size(), options.max_bytes) bytes. Line terminators: "\n", "\r\n",
// lone "\r". A final partial UTF-8 sequence cut by the size cap is dropped (not replaced). Tabs
// and other control characters are kept as is (the ui draws them). Pure.
// Errors: Unsupported (looks_binary on the first bytes), InvalidArgument (max_row_bytes < 64,
// max_rows 0 or max_bytes 0, or more than UINT32_MAX bytes of text), OutOfMemory.
[[nodiscard]] Result<TextPreview> decode_text(Span<const std::uint8_t> bytes,
                                              std::uint64_t file_size, const TextOptions& options);

// =============================================================================================
// Folder
// =============================================================================================

inline constexpr std::size_t kDefaultFolderPreviewEntries = 2000;

// The ui passes the inspecting tab's view options so the preview lists like the tab would.
struct FolderOptions {
    bool show_hidden = false;
    fs::SortOptions sort;
    std::size_t max_entries = kDefaultFolderPreviewEntries;

    friend bool operator==(const FolderOptions&, const FolderOptions&) = default;
};

// Contents of a folder, listed with fs::list_directory (stop-token overload) on a worker and
// sorted with fs::sort_entries; the Inspector shows them without navigating.
struct FolderPreview {
    std::vector<fs::DirEntry> entries; // shown entries, sorted, at most max_entries
    std::size_t total = 0;             // shown-policy entries before the cap
    std::size_t folders = 0;           // among `total` (fs::is_directory_like)
    std::size_t files = 0;             // among `total`, everything else
    std::size_t hidden = 0;            // hidden entries not shown (0 when show_hidden)
    bool truncated = false;            // total > entries.size()
};

// =============================================================================================
// Results
// =============================================================================================

enum class PreviewPurpose : std::uint8_t {
    Thumbnail = 0, // small image for a list / grid row; goes through the Freedesktop cache
    Inspector = 1, // full Inspector content (image at view size, text, folder, metadata)
};

// Where the pixels came from (diagnostics, tests, the Inspector's "from cache" hint).
enum class ImageOrigin : std::uint8_t {
    Decoded = 0,             // decoded from the file by a built-in decoder
    ThumbnailCache = 1,      // a valid Freedesktop thumbnail (ours or another program's)
    ExternalThumbnailer = 2, // produced by a /usr/share/thumbnailers entry
    EmbeddedThumbnail = 3,   // EXIF / HEIF embedded preview image
};

enum class PlaceholderReason : std::uint8_t {
    NoProvider = 0,    // no provider handles this type ("No preview available")
    ComingSoon = 1,    // PDF, and SVG without librsvg: provider interface exists, no decoder yet
    Binary = 2,        // text-like name but binary content
    NoThumbnailer = 3, // video / other type without FFmpeg and without a system thumbnailer
    KnownFailure = 4,  // a valid Freedesktop fail marker exists for this file version
    NotGenerated = 5,  // thumbnail policy: not generated here (network / removable volume,
                       // thumbnails disabled, file inside the thumbnail cache)
};

// "Nothing to show", which is not an error: the ui shows the type icon and the metadata it has.
struct PlaceholderPreview {
    PlaceholderReason reason = PlaceholderReason::NoProvider;
    std::string detail; // English developer detail (logs), never shown untranslated
};

struct ImagePreview {
    ImageBuffer image; // display orientation; fit into the request target (never upscaled)
    ImageInfo info;
    ImageOrigin origin = ImageOrigin::Decoded;
};

struct VideoPreview {
    ImageBuffer poster; // empty: no poster (show the type icon and a play disc)
    ImageOrigin poster_origin = ImageOrigin::Decoded;
    VideoInfo info;
};

// Order is part of the contract: content.index() == static_cast<size_t>(PreviewKind).
using PreviewContent =
    std::variant<PlaceholderPreview, ImagePreview, VideoPreview, TextPreview, FolderPreview>;

enum class PreviewKind : std::uint8_t {
    Unsupported = 0, // PlaceholderPreview
    Image = 1,
    Video = 2,
    Text = 3,
    Folder = 4,
};

struct PreviewResult {
    PreviewContent content;
    std::string provider_id; // IPreviewProvider::id() that produced it ("" for the registry's
                             // own NoProvider placeholder)

    [[nodiscard]] PreviewKind kind() const noexcept {
        return static_cast<PreviewKind>(content.index());
    }
};

// Bytes the result keeps alive (pixel buffers, text, row index, entries' names and the fixed
// struct sizes), the unit of the memory cache budget. Pure.
[[nodiscard]] std::size_t approx_bytes(const PreviewResult& result) noexcept;

// =============================================================================================
// Requests and cache keys
// =============================================================================================

// Volume facts the ui knows from its volume snapshot (places::containing_volume); preview never
// queries mounts itself.
struct SourceLocation {
    bool network = false;   // VolumeKind::Network (or GVfs later)
    bool removable = false; // VolumeInfo::removable

    friend constexpr bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

struct PreviewRequest {
    Path path;          // absolute; for an entry of a listing: folder / entry.name
    fs::DirEntry entry; // listing snapshot of `path` (kind, size, mtime, mode, ...)
    // MIME type, filled by the caller with mime::guess_for_entry(entry) (the ui already computes
    // it for the Type column; src/preview does not link the mime implementation). Empty is
    // treated as mime::kOctetStream. Providers may sniff content and override it (an
    // extensionless JPEG).
    std::string mime;
    PreviewPurpose purpose = PreviewPurpose::Inspector;
    // Bounding box in device pixels. Thumbnail: the row/tile icon box (the service picks the
    // Freedesktop size class with thumbnail::size_for). Inspector: the view area for "fit";
    // {0, 0} = full resolution (1:1), capped at kMaxOutputPixels.
    PixelSize target;
    SourceLocation location;
    FolderOptions folder; // used for folders only
    TextOptions text;     // used for text only
    // Cancellation. PreviewService replaces it with the token of its job; direct callers of a
    // provider or of ProviderRegistry::produce set their own.
    std::stop_token stop;
};

// Memory-cache identity (docs/architecture.md "Phase 6"). A changed file (mtime or size), a
// different target size, purpose or view option is a different entry; stale entries simply age
// out of the LRU. Symlinks: the key uses the entry metadata as listed (the link's own unless
// the listing followed links); the Freedesktop cache validates against the target (stat on the
// worker), see thumbnail.hpp.
struct PreviewKey {
    Path path;
    Timestamp mtime{};
    std::uint64_t size = 0;
    PixelSize target;
    PreviewPurpose purpose = PreviewPurpose::Inspector;
    // Hash of the options that change the result: FolderOptions for a directory entry,
    // TextOptions for others (0 for a Thumbnail). Computed by make_key().
    std::uint64_t options = 0;

    friend bool operator==(const PreviewKey&, const PreviewKey&) = default;
};

// Pure: path, entry.mtime, entry.size, target, purpose and the options hash of the request.
[[nodiscard]] PreviewKey make_key(const PreviewRequest& request);
// Pure, stable within one process run (not persisted).
[[nodiscard]] std::size_t hash_value(const PreviewKey& key) noexcept;
struct PreviewKeyHash {
    [[nodiscard]] std::size_t operator()(const PreviewKey& key) const noexcept {
        return hash_value(key);
    }
};

// =============================================================================================
// Pure geometry helpers
// =============================================================================================

// Largest size with the aspect ratio of `source` that fits in `bounds`, rounded to the nearest
// pixel, each side >= 1. Never larger than `source` unless allow_upscale. An empty `bounds`
// means "no bound" (returns `source`, or the kMaxOutputPixels-capped fit when it is larger).
// An empty `source` returns {0, 0}.
[[nodiscard]] PixelSize fit_size(PixelSize source, PixelSize bounds,
                                 bool allow_upscale = false) noexcept;

// libjpeg-turbo DCT scaling choice: the largest denominator d in {1, 2, 4, 8} such that
// ceil(stored.width / d) >= target.width and ceil(stored.height / d) >= target.height, so the
// decoder output is never smaller than the target and the final resize only shrinks. `target`
// must be in STORED orientation: callers swap it with oriented_size() for orientations 5..8
// first. An empty target returns the smallest d whose output has <= kMaxOutputPixels
// (full-resolution request), or 8 if none. An empty `stored` returns 1.
[[nodiscard]] std::uint32_t select_jpeg_scale_denom(PixelSize stored, PixelSize target) noexcept;

// Returns `image` transformed for display (see ExifOrientation): Normal returns the input
// unchanged (moved); 2..8 return a new buffer (5..8 swap width and height). The output stride
// is width * 4. `opaque` is preserved.
// Errors: InvalidArgument (!image.valid()), OutOfMemory.
[[nodiscard]] Result<ImageBuffer> apply_exif_orientation(ImageBuffer image,
                                                         ExifOrientation orientation);

// Area-averaging (box) downscale of a premultiplied buffer to exactly `size` (each side <= the
// source's; equal size returns a copy). Used by providers after DCT scaling, by the thumbnail
// writer and by the ui for nothing (the GPU scales on draw).
// Errors: InvalidArgument (!image.valid(), empty size, or a side larger than the source),
// OutOfMemory.
[[nodiscard]] Result<ImageBuffer> downscale(const ImageBuffer& image, PixelSize size);

// =============================================================================================
// Providers
// =============================================================================================

// can_handle() priorities. Higher wins; ties go to the provider added first.
inline constexpr int kCannotHandle = 0;
inline constexpr int kPriorityFallback = 10;  // placeholders ("coming soon", no thumbnailer)
inline constexpr int kPriorityExternal = 30;  // system .thumbnailer entries
inline constexpr int kPriorityGeneric = 50;   // text for any text/* or unknown small file
inline constexpr int kPriorityNative = 100;   // built-in decoders for their formats
inline constexpr int kPriorityOverride = 200; // reserved for plug-ins that replace a built-in

// A source of previews for some file types. Built-in providers (preview_service.hpp) and later
// plug-ins (plugin.h) implement it.
//
// Thread-safety: one provider object is shared by all preview workers. can_handle() must be
// pure (no I/O; extension / MIME / entry kind only) and is called on the UI thread or workers.
// produce() is called concurrently from several worker threads for different requests and must
// be reentrant: no unsynchronized mutable state. It may block on file I/O and child processes,
// must check request.stop regularly (see "Untrusted input") and return Cancelled promptly
// (within about 50 ms of CPU work, or as soon as a blocking read returns).
class IPreviewProvider {
public:
    IPreviewProvider() = default;
    virtual ~IPreviewProvider() = default;
    IPreviewProvider(const IPreviewProvider&) = delete;
    IPreviewProvider& operator=(const IPreviewProvider&) = delete;
    IPreviewProvider(IPreviewProvider&&) = delete;
    IPreviewProvider& operator=(IPreviewProvider&&) = delete;

    // Stable id, e.g. "image.jpeg", "text.utf8", "video.ffmpeg", "thumbnailer.ffmpegthumbnailer".
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // Priority for this file, or kCannotHandle. `mime` is never empty (the registry passes
    // application/octet-stream for an empty request MIME).
    [[nodiscard]] virtual int can_handle(std::string_view mime, const fs::DirEntry& entry,
                                         PreviewPurpose purpose) const noexcept = 0;

    // Produces the preview. Errors as listed at the top of this header; Unsupported lets the
    // registry fall through to the next candidate (e.g. a ".jpg" that is really a video).
    [[nodiscard]] virtual Result<PreviewResult> produce(const PreviewRequest& request) const = 0;
};

class ProviderRegistry {
public:
    ProviderRegistry() = default;

    // Errors: InvalidArgument (null provider or empty id), AlreadyExists (same id).
    Result<void> add(std::unique_ptr<IPreviewProvider> provider);

    [[nodiscard]] std::size_t size() const noexcept { return providers_.size(); }
    // nullptr when no provider has this id.
    [[nodiscard]] const IPreviewProvider* find(std::string_view id) const noexcept;

    // Providers with can_handle() > kCannotHandle, highest priority first, ties in add order.
    // `mime` empty: application/octet-stream. Pure.
    [[nodiscard]] std::vector<const IPreviewProvider*>
    candidates(std::string_view mime, const fs::DirEntry& entry, PreviewPurpose purpose) const;

    // Tries candidates() in order: the first success wins (provider_id set); Unsupported tries
    // the next; any other error is returned at once. No candidate, or every candidate
    // Unsupported: success with PlaceholderPreview{NoProvider} and provider_id "". Blocking
    // (calls produce()); worker threads.
    // Errors: those of the providers except Unsupported; Cancelled when request.stop fired.
    [[nodiscard]] Result<PreviewResult> produce(const PreviewRequest& request) const;

private:
    std::vector<std::unique_ptr<IPreviewProvider>> providers_;
};

} // namespace navigator::preview
