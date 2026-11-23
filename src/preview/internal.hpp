// src/preview/internal.hpp: shared internals of navigator_preview (not a public header).
//
// - DecodeParams and the per-format file decoders (decode_*.cpp), each bounded by the limits in
//   navigator/preview.hpp and honouring the stop token.
// - RowScaler: streaming area-average (box) downscaler fed one source row at a time, top to
//   bottom, holding only one accumulator row (never the whole source image).
// - File helpers (bounded reads, errno mapping) and the EXIF parser.
// Thread-safety: everything here is reentrant; objects are single-thread-at-a-time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/preview.hpp>
#include <navigator/types.hpp>

namespace navigator::preview::detail {

// ---------------------------------------------------------------------------------------------
// Errors and files
// ---------------------------------------------------------------------------------------------

// errno -> ErrorCode (ENOENT/ENOTDIR NotFound, EACCES/EPERM PermissionDenied, ENOMEM OutOfMemory,
// else IoError) with "<what>: <path>: <strerror>".
[[nodiscard]] Error errno_error(int err, std::string_view what, const Path& path);

// RAII file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept;
    ~Fd();

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_ = -1;
};

// Opens a regular file read-only (O_CLOEXEC | O_NOCTTY, non-blocking open so a FIFO swapped in
// cannot hang the worker, then blocking again). `size` receives st_size.
// Errors: NotFound, PermissionDenied, InvalidArgument (not a regular file), IoError.
[[nodiscard]] Result<Fd> open_regular(const Path& path, std::uint64_t* size, bool nofollow = false);

// Reads up to `max_bytes` from the start (pread loop, stop checked every 1 MiB).
// Errors: Cancelled, IoError, OutOfMemory.
[[nodiscard]] Result<std::vector<std::uint8_t>>
read_prefix(int fd, std::uint64_t max_bytes, const std::stop_token& stop, const Path& path);

// pread loop at `offset`; returns bytes read (short at EOF). -1 on error (errno set).
[[nodiscard]] long long pread_full(int fd, void* buf, std::size_t len, std::uint64_t offset);

// ---------------------------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------------------------

struct DecodeParams {
    PixelSize target; // display-orientation bounds; empty = full resolution (output capped)
    PreviewPurpose purpose = PreviewPurpose::Inspector;
    std::stop_token stop;
    std::uint64_t memory_budget = kDefaultDecodeMemoryBytes;
    // Largest source that may be decoded whole (formats without streaming / scaled decode).
    std::uint64_t full_decode_pixels = kMaxFullDecodePixels;
};

[[nodiscard]] inline std::uint64_t full_decode_limit(const DecodeParams& p) noexcept {
    const std::uint64_t by_budget = p.memory_budget / 4U;
    return p.full_decode_pixels < by_budget ? p.full_decode_pixels : by_budget;
}

// Rejects sizes over kMaxSourceDimension / kMaxSourcePixels (InvalidArgument) and empty sizes.
[[nodiscard]] Result<void> check_source_size(PixelSize stored, std::string_view what);

// Output size in display orientation for a source, then converted to stored orientation.
[[nodiscard]] PixelSize output_display_size(PixelSize display, PixelSize target) noexcept;

[[nodiscard]] Result<ImagePreview> decode_jpeg_file(const Path& path, const DecodeParams& p);
[[nodiscard]] Result<ImagePreview> decode_png_file(const Path& path, const DecodeParams& p);
[[nodiscard]] Result<ImagePreview> decode_webp_file(const Path& path, const DecodeParams& p);
[[nodiscard]] Result<ImagePreview> decode_gif_file(const Path& path, const DecodeParams& p);
[[nodiscard]] Result<ImagePreview> decode_bmp_file(const Path& path, const DecodeParams& p);
[[nodiscard]] Result<ImagePreview> decode_heif_file(const Path& path, const DecodeParams& p);
[[nodiscard]] Result<ImagePreview> decode_svg_file(const Path& path, const DecodeParams& p);
[[nodiscard]] Result<VideoPreview> decode_video_file(const Path& path, const DecodeParams& p);

// Sniffs the first bytes and dispatches to the decoder of the format. Unsupported for formats
// without a decoder in this build (Tiff, Ico, unknown, HEIF/SVG when not built).
[[nodiscard]] Result<ImagePreview> decode_image_file(const Path& path, const DecodeParams& p,
                                                     bool nofollow = false);

// Sniffed format of a file (reads 64 bytes). Unknown on read errors.
[[nodiscard]] ImageFormat sniff_file(const Path& path) noexcept;

// Runtime decoder availability of libheif (HEVC / AV1); false when not built with libheif.
[[nodiscard]] bool heif_has_hevc() noexcept;
[[nodiscard]] bool heif_has_av1() noexcept;

// ---------------------------------------------------------------------------------------------
// PNG core (shared by the image decoder, the thumbnail reader and thumbnailer output)
// ---------------------------------------------------------------------------------------------

struct PngHeader {
    PixelSize size;
    std::uint8_t bit_depth = 8;
    bool has_alpha = false;
    bool interlaced = false;
    bool has_icc = false;
    std::uint32_t frames = 1;                              // acTL num_frames when present
    std::vector<std::pair<std::string, std::string>> text; // tEXt/zTXt/iTXt before IDAT
    std::vector<std::uint8_t> exif;                        // eXIf payload (raw TIFF)
};

enum class PngHeaderVerdict : std::uint8_t { Decode, Stop };

struct PngDecodeOptions {
    PixelSize out_stored; // exact output size (stored orientation); empty: computed by caller
    bool header_only = false;
    std::uint64_t full_decode_pixels = kMaxFullDecodePixels; // interlaced sources
    std::uint32_t max_dimension = kMaxSourceDimension;
};

struct PngDecoded {
    PngHeader header;
    ImageBuffer image; // stored orientation, premultiplied; empty when header_only
    bool partial = false;
};

// Decodes PNG bytes from `fd`. `choose_size` is called after the header with the header and
// returns the output size (stored orientation) or an empty size to stop after the header.
// Errors: InvalidArgument (not a PNG, corrupt before any row, limits), Cancelled, OutOfMemory,
// IoError.
using PngSizeChooser = PixelSize (*)(const PngHeader& header, void* user);
[[nodiscard]] Result<PngDecoded> png_decode_fd(int fd, const PngDecodeOptions& options,
                                               PngSizeChooser choose_size, void* user,
                                               const std::stop_token& stop);

// Encodes straight-alpha RGBA rows (from a premultiplied buffer) to a PNG file `fd` with tEXt
// chunks. Errors: IoError, OutOfMemory.
[[nodiscard]] Result<void>
png_encode_fd(int fd, const ImageBuffer& premultiplied,
              const std::vector<std::pair<std::string, std::string>>& text);

// ---------------------------------------------------------------------------------------------
// Streaming downscaler
// ---------------------------------------------------------------------------------------------

class RowScaler {
public:
    // src >= dst on both axes (dst clamped to src). src_premultiplied false: rows are straight
    // alpha and are premultiplied on input.
    [[nodiscard]] static Result<RowScaler> create(PixelSize src, PixelSize dst,
                                                  bool src_premultiplied);

    // Next source row (src.width RGBA8 pixels). Rows past src.height are ignored.
    void push_row(const std::uint8_t* rgba) noexcept;
    // `count` fully transparent rows (cheap: no horizontal work).
    void push_blank_rows(std::uint32_t count) noexcept;
    [[nodiscard]] std::uint32_t rows_pushed() const noexcept { return rows_in_; }
    [[nodiscard]] std::uint32_t source_height() const noexcept { return src_.height; }
    // Completes the image; missing rows are transparent.
    [[nodiscard]] ImageBuffer finish() noexcept;

private:
    RowScaler() = default;
    void emit_row() noexcept;

    PixelSize src_;
    PixelSize dst_;
    bool premultiplied_ = true;
    bool identity_x_ = false;
    std::uint32_t rows_in_ = 0;
    std::uint32_t row_out_ = 0;
    double row_weight_ = 0.0;
    struct XMap {
        std::uint32_t j0 = 0;
        float w0 = 1.0F;
        float w1 = 0.0F; // contribution to j0 + 1
    };
    std::vector<XMap> xmap_;
    std::vector<float> xnorm_; // 1 / horizontal weight per target column
    std::vector<float> hrow_;
    std::vector<float> acc_;
    ImageBuffer out_;
    bool opaque_ = true;
};

// Premultiplies straight RGBA in place.
void premultiply_row(std::uint8_t* rgba, std::size_t pixels) noexcept;

// Builds the result from a fully decoded straight or premultiplied stored-orientation buffer:
// scales to fit_size(display, target) and applies the orientation.
[[nodiscard]] Result<ImageBuffer> scale_and_orient(const std::uint8_t* pixels, PixelSize stored,
                                                   std::size_t stride, bool premultiplied,
                                                   ExifOrientation orientation, PixelSize target);

// ---------------------------------------------------------------------------------------------
// EXIF
// ---------------------------------------------------------------------------------------------

// Parses EXIF from a block that starts with "Exif\0\0" + TIFF, or directly with a TIFF header.
// nullopt when nothing parsable. Input over 4 MiB is ignored.
[[nodiscard]] std::optional<ExifInfo> parse_exif(const std::uint8_t* data, std::size_t size);

// Repairs to valid UTF-8 (U+FFFD), trims spaces / NULs, caps at 256 bytes.
[[nodiscard]] std::string clean_exif_string(std::string_view raw);

// Append a code point as UTF-8.
void append_utf8(std::string& out, std::uint32_t cp);

} // namespace navigator::preview::detail
