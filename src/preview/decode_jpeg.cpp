// src/preview/decode_jpeg.cpp: JPEG through libjpeg(-turbo) with DCT scaling, a streaming
// source with stop checks, a memory cap (max_memory_to_use), a scan-count cap for progressive
// files and EXIF orientation. libjpeg errors longjmp back into run_decode() only; all C++ state
// lives in JpegCtx, created before setjmp.
#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <new>

#include <jerror.h>
#include <jpeglib.h>

#include "internal.hpp"

namespace navigator::preview::detail {

namespace {

constexpr std::size_t kInputChunk = std::size_t{64} * 1024U;
constexpr int kMaxScans = 1000;
constexpr int kMaxWarnings = 10000;

struct JpegCtx;

struct ErrorMgr {
    jpeg_error_mgr pub{};
    std::jmp_buf jump{};
    JpegCtx* ctx = nullptr;
};

struct SourceMgr {
    jpeg_source_mgr pub{};
    JpegCtx* ctx = nullptr;
};

struct JpegCtx {
    jpeg_decompress_struct cinfo{};
    ErrorMgr err;
    SourceMgr src;
    jpeg_progress_mgr progress{};
    int fd = -1;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> buffer;
    std::stop_token stop;
    bool cancelled = false;
    bool io_error = false;
    int read_errno = 0;
    bool out_of_memory = false;
    bool eof = false;
    int warnings = 0;
    char message[JMSG_LENGTH_MAX] = {};
    bool created = false;

    // Decode state (filled inside run_decode()).
    PixelSize target;
    std::uint64_t memory_budget = 0;
    ImageInfo info;
    std::vector<std::uint8_t> exif;
    std::optional<RowScaler> scaler;
    std::vector<std::uint8_t> rows;
    std::vector<std::uint8_t> cmyk_row;
    bool header_done = false;
    bool cmyk = false;
    bool adobe_inverted = false;
    std::uint32_t lines_read = 0;

    ~JpegCtx() {
        if (created) {
            jpeg_destroy_decompress(&cinfo);
        }
    }
};

[[noreturn]] void on_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<ErrorMgr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->ctx->message);
    if (cinfo->err->msg_code == JERR_OUT_OF_MEMORY ||
        cinfo->err->msg_code == JERR_NO_BACKING_STORE) {
        err->ctx->out_of_memory = true;
    }
    std::longjmp(err->jump, 1);
}

void on_emit_message(j_common_ptr cinfo, int level) {
    auto* err = reinterpret_cast<ErrorMgr*>(cinfo->err);
    if (level < 0) {
        ++err->ctx->warnings;
        if (err->ctx->warnings > kMaxWarnings) {
            std::strncpy(err->ctx->message, "too many corrupt-data warnings",
                         sizeof(err->ctx->message) - 1);
            std::longjmp(err->jump, 1);
        }
    }
}

void on_output_message(j_common_ptr /*cinfo*/) {}

void on_progress(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<ErrorMgr*>(cinfo->err);
    JpegCtx* ctx = err->ctx;
    if (ctx->stop.stop_requested()) {
        ctx->cancelled = true;
        std::longjmp(err->jump, 1);
    }
    if (cinfo->is_decompressor != 0) {
        auto* d = reinterpret_cast<j_decompress_ptr>(cinfo);
        if (d->input_scan_number > kMaxScans) {
            std::strncpy(ctx->message, "too many progressive scans", sizeof(ctx->message) - 1);
            std::longjmp(err->jump, 1);
        }
    }
}

void src_init(j_decompress_ptr /*cinfo*/) {}

boolean src_fill(j_decompress_ptr cinfo) {
    auto* src = reinterpret_cast<SourceMgr*>(cinfo->src);
    JpegCtx* ctx = src->ctx;
    if (ctx->stop.stop_requested()) {
        ctx->cancelled = true;
        std::longjmp(ctx->err.jump, 1);
    }
    long long n = 0;
    if (!ctx->eof) {
        n = pread_full(ctx->fd, ctx->buffer.data(), ctx->buffer.size(), ctx->offset);
        if (n < 0) {
            ctx->io_error = true;
            ctx->read_errno = errno;
            std::longjmp(ctx->err.jump, 1);
        }
    }
    if (n <= 0) {
        // Premature end: insert a fake EOI (libjpeg then pads the rest of the image).
        ctx->eof = true;
        ++ctx->warnings;
        ctx->buffer[0] = 0xFF;
        ctx->buffer[1] = JPEG_EOI;
        n = 2;
        if (ctx->warnings > kMaxWarnings) {
            std::strncpy(ctx->message, "truncated", sizeof(ctx->message) - 1);
            std::longjmp(ctx->err.jump, 1);
        }
    } else {
        ctx->offset += static_cast<std::uint64_t>(n);
    }
    src->pub.next_input_byte = ctx->buffer.data();
    src->pub.bytes_in_buffer = static_cast<std::size_t>(n);
    return TRUE;
}

void src_skip(j_decompress_ptr cinfo, long num_bytes) {
    auto* src = reinterpret_cast<SourceMgr*>(cinfo->src);
    if (num_bytes <= 0) {
        return;
    }
    auto remaining = static_cast<std::size_t>(num_bytes);
    while (remaining > src->pub.bytes_in_buffer) {
        remaining -= src->pub.bytes_in_buffer;
        (void)src_fill(cinfo);
    }
    src->pub.next_input_byte += remaining;
    src->pub.bytes_in_buffer -= remaining;
}

void src_term(j_decompress_ptr /*cinfo*/) {}

// Converts one CMYK / inverted-CMYK (Adobe) row to RGBA.
void cmyk_to_rgba(const std::uint8_t* in, std::uint8_t* out, std::uint32_t width,
                  bool inverted) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        unsigned c = in[std::size_t{x} * 4U];
        unsigned m = in[std::size_t{x} * 4U + 1U];
        unsigned y = in[std::size_t{x} * 4U + 2U];
        unsigned k = in[std::size_t{x} * 4U + 3U];
        if (!inverted) {
            c = 255U - c;
            m = 255U - m;
            y = 255U - y;
            k = 255U - k;
        }
        out[std::size_t{x} * 4U] = static_cast<std::uint8_t>(c * k / 255U);
        out[std::size_t{x} * 4U + 1U] = static_cast<std::uint8_t>(m * k / 255U);
        out[std::size_t{x} * 4U + 2U] = static_cast<std::uint8_t>(y * k / 255U);
        out[std::size_t{x} * 4U + 3U] = 255;
    }
}

// All libjpeg calls happen here. Returns 0 on success, 1 after a longjmp, 2 on a decode-time
// error recorded in ctx.message (limits).
int run_decode(JpegCtx& ctx) {
    if (setjmp(ctx.err.jump) != 0) {
        return 1;
    }
    jpeg_create_decompress(&ctx.cinfo);
    ctx.created = true;
    ctx.cinfo.mem->max_memory_to_use = static_cast<long>(ctx.memory_budget);
    ctx.cinfo.src = &ctx.src.pub;
    ctx.cinfo.progress = &ctx.progress;
    jpeg_save_markers(&ctx.cinfo, JPEG_APP0 + 1, 0xFFFF);
    jpeg_save_markers(&ctx.cinfo, JPEG_APP0 + 2, 16);
    (void)jpeg_read_header(&ctx.cinfo, TRUE);

    const PixelSize stored{ctx.cinfo.image_width, ctx.cinfo.image_height};
    if (!check_source_size(stored, "jpeg")) {
        std::strncpy(ctx.message, "declared size exceeds the limits", sizeof(ctx.message) - 1);
        return 2;
    }
    ctx.info.format = ImageFormat::Jpeg;
    ctx.info.stored = stored;
    ctx.info.bits_per_channel = static_cast<std::uint8_t>(ctx.cinfo.data_precision);
    for (jpeg_saved_marker_ptr m = ctx.cinfo.marker_list; m != nullptr; m = m->next) {
        if (m->marker == JPEG_APP0 + 1 && m->data_length > 6 &&
            std::memcmp(m->data, "Exif\0\0", 6) == 0 && ctx.exif.empty()) {
            ctx.exif.assign(m->data, m->data + m->data_length);
        } else if (m->marker == JPEG_APP0 + 2 && m->data_length >= 12 &&
                   std::memcmp(m->data, "ICC_PROFILE", 11) == 0) {
            ctx.info.has_icc_profile = true;
        }
    }
    ctx.header_done = true;
    if (!ctx.exif.empty()) {
        ctx.info.exif = parse_exif(ctx.exif.data(), ctx.exif.size());
        if (ctx.info.exif) {
            ctx.info.orientation = ctx.info.exif->orientation;
        }
    }
    ctx.info.display = oriented_size(stored, ctx.info.orientation);

    const PixelSize out_display = output_display_size(ctx.info.display, ctx.target);
    const PixelSize out_stored = oriented_size(out_display, ctx.info.orientation);
    const std::uint32_t denom = select_jpeg_scale_denom(stored, out_stored);
    ctx.cinfo.scale_num = 1;
    ctx.cinfo.scale_denom = denom;
    ctx.cmyk = ctx.cinfo.jpeg_color_space == JCS_CMYK || ctx.cinfo.jpeg_color_space == JCS_YCCK;
    ctx.adobe_inverted = ctx.cmyk && ctx.cinfo.saw_Adobe_marker != 0;
    ctx.cinfo.out_color_space = ctx.cmyk ? JCS_CMYK : JCS_EXT_RGBA;
    ctx.cinfo.dct_method = JDCT_ISLOW;
    jpeg_calc_output_dimensions(&ctx.cinfo);
    const PixelSize decoded{ctx.cinfo.output_width, ctx.cinfo.output_height};
    if (decoded.empty()) {
        std::strncpy(ctx.message, "empty output", sizeof(ctx.message) - 1);
        return 2;
    }
    const std::uint64_t out_bytes = std::uint64_t{decoded.width} * 4U * 16U;
    if (out_bytes > ctx.memory_budget) {
        ctx.out_of_memory = true;
        return 2;
    }
    PixelSize scaled = out_stored;
    scaled.width = std::min(scaled.width, decoded.width);
    scaled.height = std::min(scaled.height, decoded.height);
    auto scaler = RowScaler::create(decoded, scaled, true);
    if (!scaler) {
        ctx.out_of_memory = scaler.error().code == ErrorCode::OutOfMemory;
        std::strncpy(ctx.message, scaler.error().message.c_str(), sizeof(ctx.message) - 1);
        return 2;
    }
    ctx.scaler.emplace(std::move(*scaler));
    (void)jpeg_start_decompress(&ctx.cinfo);
    constexpr std::uint32_t kBatch = 16;
    const std::size_t row_bytes = std::size_t{ctx.cinfo.output_width} * 4U;
    ctx.rows.resize(row_bytes * kBatch);
    if (ctx.cmyk) {
        ctx.cmyk_row.resize(row_bytes);
    }
    JSAMPROW pointers[kBatch];
    while (ctx.cinfo.output_scanline < ctx.cinfo.output_height) {
        const std::uint32_t want =
            std::min<std::uint32_t>(kBatch, ctx.cinfo.output_height - ctx.cinfo.output_scanline);
        for (std::uint32_t i = 0; i < want; ++i) {
            pointers[i] = ctx.rows.data() + row_bytes * i;
        }
        const JDIMENSION got = jpeg_read_scanlines(&ctx.cinfo, pointers, want);
        if (got == 0) {
            break;
        }
        for (JDIMENSION i = 0; i < got; ++i) {
            std::uint8_t* row = ctx.rows.data() + row_bytes * i;
            if (ctx.cmyk) {
                std::memcpy(ctx.cmyk_row.data(), row, row_bytes);
                cmyk_to_rgba(ctx.cmyk_row.data(), row, ctx.cinfo.output_width, ctx.adobe_inverted);
            }
            ctx.scaler->push_row(row);
        }
        ctx.lines_read += got;
        if (ctx.stop.stop_requested()) {
            ctx.cancelled = true;
            return 1;
        }
    }
    jpeg_abort_decompress(&ctx.cinfo);
    return 0;
}

} // namespace

Result<ImagePreview> decode_jpeg_file(const Path& path, const DecodeParams& p) {
    std::uint64_t file_size = 0;
    auto fd = open_regular(path, &file_size);
    if (!fd) {
        return Unexpected<Error>(fd.error());
    }
    try {
        auto ctx = std::make_unique<JpegCtx>();
        ctx->fd = fd->get();
        ctx->buffer.resize(kInputChunk);
        ctx->stop = p.stop;
        ctx->target = p.target;
        ctx->memory_budget = p.memory_budget;
        ctx->cinfo.err = jpeg_std_error(&ctx->err.pub);
        ctx->err.pub.error_exit = on_error_exit;
        ctx->err.pub.emit_message = on_emit_message;
        ctx->err.pub.output_message = on_output_message;
        ctx->err.ctx = ctx.get();
        ctx->src.ctx = ctx.get();
        ctx->src.pub.init_source = src_init;
        ctx->src.pub.fill_input_buffer = src_fill;
        ctx->src.pub.skip_input_data = src_skip;
        ctx->src.pub.resync_to_restart = jpeg_resync_to_restart;
        ctx->src.pub.term_source = src_term;
        ctx->progress.progress_monitor = on_progress;

        const int rc = run_decode(*ctx);
        if (ctx->cancelled || p.stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        if (ctx->io_error) {
            return Unexpected<Error>(errno_error(ctx->read_errno, "read", path));
        }
        if (rc != 0) {
            if (ctx->out_of_memory) {
                return fail(ErrorCode::OutOfMemory,
                            "jpeg: decode exceeds the memory budget: " + path.native());
            }
            // A partial image is returned when rows were decoded before the error.
            if (!ctx->scaler || ctx->lines_read == 0) {
                return fail(ErrorCode::InvalidArgument,
                            std::string("jpeg: ") + ctx->message + ": " + path.native());
            }
        }
        if (!ctx->scaler.has_value()) {
            return fail(ErrorCode::InvalidArgument, "jpeg: no image data: " + path.native());
        }
        RowScaler& scaler = ctx->scaler.value(); // NOLINT(bugprone-unchecked-optional-access)
        ImageBuffer stored = scaler.finish();
        stored.opaque = stored.opaque && rc == 0;
        auto oriented = apply_exif_orientation(std::move(stored), ctx->info.orientation);
        if (!oriented) {
            return Unexpected<Error>(oriented.error());
        }
        ImagePreview out;
        out.image = std::move(*oriented);
        out.info = std::move(ctx->info);
        out.origin = ImageOrigin::Decoded;
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "jpeg: allocation failed");
    }
}

} // namespace navigator::preview::detail
