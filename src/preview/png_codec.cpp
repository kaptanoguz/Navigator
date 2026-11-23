// src/preview/png_codec.cpp: libpng decoding (streaming rows into a RowScaler; interlaced files
// decoded whole up to the full-decode limit), the header / tEXt reader used by the Freedesktop
// cache, and the PNG encoder for thumbnails. libpng errors longjmp back into the run_* functions
// only; all C++ state is created before setjmp.
#include <cerrno>
#include <csetjmp>
#include <cstring>
#include <new>

#include <png.h>
#include <unistd.h>

#include "internal.hpp"

namespace navigator::preview::detail {

namespace {

constexpr std::size_t kChunkMallocMax = std::size_t{8} << 20U;

struct PngReadCtx {
    png_structp png = nullptr;
    png_infop info = nullptr;
    int fd = -1;
    std::uint64_t offset = 0;
    std::stop_token stop;
    bool cancelled = false;
    bool io_error = false;
    int read_errno = 0;
    std::string message;
    const PngDecodeOptions* options = nullptr;
    PngSizeChooser choose = nullptr;
    void* user = nullptr;

    PngDecoded result;
    std::optional<RowScaler> scaler;
    std::vector<std::uint8_t> row;
    std::vector<std::uint8_t> full;
    std::uint32_t rows_done = 0;
    bool limit_exceeded = false;
    bool oom = false;

    ~PngReadCtx() {
        if (png != nullptr) {
            png_destroy_read_struct(&png, info != nullptr ? &info : nullptr, nullptr);
        }
    }
};

void on_png_error(png_structp png, png_const_charp msg) {
    auto* ctx = static_cast<PngReadCtx*>(png_get_error_ptr(png));
    if (ctx != nullptr && msg != nullptr && ctx->message.empty()) {
        ctx->message = msg;
    }
    png_longjmp(png, 1);
}

void on_png_warning(png_structp /*png*/, png_const_charp /*msg*/) {}

void on_png_read(png_structp png, png_bytep data, png_size_t length) {
    auto* ctx = static_cast<PngReadCtx*>(png_get_io_ptr(png));
    if (ctx->stop.stop_requested()) {
        ctx->cancelled = true;
        png_error(png, "cancelled");
    }
    const long long n = pread_full(ctx->fd, data, length, ctx->offset);
    if (n < 0) {
        ctx->io_error = true;
        ctx->read_errno = errno;
        png_error(png, "read error");
    }
    if (static_cast<std::size_t>(n) < length) {
        png_error(png, "truncated file");
    }
    ctx->offset += static_cast<std::uint64_t>(n);
}

int on_user_chunk(png_structp png, png_unknown_chunkp chunk) {
    auto* ctx = static_cast<PngReadCtx*>(png_get_user_chunk_ptr(png));
    if (std::memcmp(chunk->name, "acTL", 4) == 0 && chunk->size >= 8) {
        const png_bytep d = chunk->data;
        const std::uint32_t frames = (std::uint32_t{d[0]} << 24U) | (std::uint32_t{d[1]} << 16U) |
                                     (std::uint32_t{d[2]} << 8U) | std::uint32_t{d[3]};
        ctx->result.header.frames = frames == 0 ? 1 : frames;
        return 1;
    }
    return 0;
}

// Returns 0 on success, 1 after a libpng error.
int run_png_read(PngReadCtx& ctx) {
    if (setjmp(png_jmpbuf(ctx.png)) != 0) {
        return 1;
    }
    png_set_read_fn(ctx.png, &ctx, on_png_read);
    png_set_user_limits(ctx.png, ctx.options->max_dimension, ctx.options->max_dimension);
    png_set_chunk_cache_max(ctx.png, 1000);
    png_set_chunk_malloc_max(ctx.png, kChunkMallocMax);
    png_set_read_user_chunk_fn(ctx.png, &ctx, on_user_chunk);
    static const png_byte kActl[] = {'a', 'c', 'T', 'L', '\0'};
    png_set_keep_unknown_chunks(ctx.png, PNG_HANDLE_CHUNK_ALWAYS, kActl, 1);
    png_read_info(ctx.png, ctx.info);

    PngHeader& h = ctx.result.header;
    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace = 0;
    png_get_IHDR(ctx.png, ctx.info, &width, &height, &bit_depth, &color_type, &interlace, nullptr,
                 nullptr);
    h.size = {width, height};
    h.bit_depth = static_cast<std::uint8_t>(bit_depth);
    h.has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 ||
                  png_get_valid(ctx.png, ctx.info, PNG_INFO_tRNS) != 0;
    h.interlaced = interlace != PNG_INTERLACE_NONE;
    h.has_icc = png_get_valid(ctx.png, ctx.info, PNG_INFO_iCCP) != 0;
    png_textp text = nullptr;
    int num_text = 0;
    if (png_get_text(ctx.png, ctx.info, &text, &num_text) > 0 && text != nullptr) {
        for (int i = 0; i < num_text && i < 256; ++i) {
            if (text[i].key == nullptr) {
                continue;
            }
            const std::size_t len = text[i].compression >= PNG_ITXT_COMPRESSION_NONE
                                        ? text[i].itxt_length
                                        : text[i].text_length;
            h.text.emplace_back(text[i].key, text[i].text != nullptr
                                                 ? std::string(text[i].text, len)
                                                 : std::string());
        }
    }
    png_uint_32 exif_len = 0;
    png_bytep exif = nullptr;
    if (png_get_eXIf_1(ctx.png, ctx.info, &exif_len, &exif) != 0 && exif != nullptr &&
        exif_len > 0) {
        h.exif.assign(exif, exif + exif_len);
    }
    if (ctx.options->header_only) {
        return 0;
    }
    const PixelSize out = ctx.choose(h, ctx.user);
    if (out.empty()) {
        return 0;
    }

    // Transforms to 8-bit RGBA (straight alpha). Gamma is ignored (sRGB assumed).
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(ctx.png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(ctx.png);
    }
    if (png_get_valid(ctx.png, ctx.info, PNG_INFO_tRNS) != 0) {
        png_set_tRNS_to_alpha(ctx.png);
    }
    if (bit_depth == 16) {
        png_set_scale_16(ctx.png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(ctx.png);
    }
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0) {
        png_set_add_alpha(ctx.png, 0xFF, PNG_FILLER_AFTER);
    }
    const int passes = png_set_interlace_handling(ctx.png);
    png_read_update_info(ctx.png, ctx.info);
    if (png_get_rowbytes(ctx.png, ctx.info) != std::size_t{width} * 4U) {
        ctx.message = "unexpected row size";
        return 1;
    }
    auto scaler = RowScaler::create(h.size, out, false);
    if (!scaler) {
        ctx.oom = scaler.error().code == ErrorCode::OutOfMemory;
        ctx.message = scaler.error().message;
        return 1;
    }
    ctx.scaler.emplace(std::move(*scaler));
    if (passes > 1) {
        if (h.size.pixels() > ctx.options->full_decode_pixels) {
            ctx.limit_exceeded = true;
            ctx.message = "interlaced image too large to decode";
            return 1;
        }
        ctx.full.resize(static_cast<std::size_t>(h.size.pixels() * 4U));
        for (int pass = 0; pass < passes; ++pass) {
            for (std::uint32_t y = 0; y < height; ++y) {
                png_read_row(ctx.png, ctx.full.data() + std::size_t{y} * width * 4U, nullptr);
            }
        }
        for (std::uint32_t y = 0; y < height; ++y) {
            ctx.scaler->push_row(ctx.full.data() + std::size_t{y} * width * 4U);
        }
        ctx.rows_done = height;
        return 0;
    }
    ctx.row.resize(std::size_t{width} * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        png_read_row(ctx.png, ctx.row.data(), nullptr);
        ctx.scaler->push_row(ctx.row.data());
        ++ctx.rows_done;
        if ((y & 63U) == 0U && ctx.stop.stop_requested()) {
            ctx.cancelled = true;
            return 1;
        }
    }
    return 0;
}

struct PngWriteCtx {
    png_structp png = nullptr;
    png_infop info = nullptr;
    int fd = -1;
    bool io_error = false;
    int write_errno = 0;
    std::string message;
    std::vector<std::uint8_t> row;
    std::vector<png_text> texts;

    ~PngWriteCtx() {
        if (png != nullptr) {
            png_destroy_write_struct(&png, info != nullptr ? &info : nullptr);
        }
    }
};

void on_png_write_error(png_structp png, png_const_charp msg) {
    auto* ctx = static_cast<PngWriteCtx*>(png_get_error_ptr(png));
    if (ctx != nullptr && msg != nullptr && ctx->message.empty()) {
        ctx->message = msg;
    }
    png_longjmp(png, 1);
}

void on_png_write(png_structp png, png_bytep data, png_size_t length) {
    auto* ctx = static_cast<PngWriteCtx*>(png_get_io_ptr(png));
    std::size_t done = 0;
    while (done < length) {
        const ssize_t n = ::write(ctx->fd, data + done, length - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ctx->io_error = true;
            ctx->write_errno = errno;
            png_error(png, "write error");
        }
        done += static_cast<std::size_t>(n);
    }
}

void on_png_flush(png_structp /*png*/) {}

int run_png_write(PngWriteCtx& ctx, const ImageBuffer& image) {
    if (setjmp(png_jmpbuf(ctx.png)) != 0) {
        return 1;
    }
    png_set_write_fn(ctx.png, &ctx, on_png_write, on_png_flush);
    png_set_IHDR(ctx.png, ctx.info, image.width, image.height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(ctx.png, 6);
    if (!ctx.texts.empty()) {
        png_set_text(ctx.png, ctx.info, ctx.texts.data(), static_cast<int>(ctx.texts.size()));
    }
    png_write_info(ctx.png, ctx.info);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const std::uint8_t* src = image.pixels.data() + std::size_t{y} * image.stride;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::uint8_t* p = src + std::size_t{x} * 4U;
            std::uint8_t* d = ctx.row.data() + std::size_t{x} * 4U;
            const unsigned a = p[3];
            d[3] = p[3];
            for (int c = 0; c < 3; ++c) {
                if (a == 255U) {
                    d[c] = p[c];
                } else if (a == 0U) {
                    d[c] = 0;
                } else {
                    const unsigned v = (p[c] * 255U + a / 2U) / a;
                    d[c] = static_cast<std::uint8_t>(v > 255U ? 255U : v);
                }
            }
        }
        png_write_row(ctx.png, ctx.row.data());
    }
    png_write_end(ctx.png, ctx.info);
    return 0;
}

} // namespace

Result<PngDecoded> png_decode_fd(int fd, const PngDecodeOptions& options,
                                 PngSizeChooser choose_size, void* user,
                                 const std::stop_token& stop) {
    try {
        std::uint8_t sig[8] = {};
        if (pread_full(fd, sig, sizeof(sig), 0) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
            return fail(ErrorCode::InvalidArgument, "png: not a PNG file");
        }
        auto ctx = std::make_unique<PngReadCtx>();
        ctx->fd = fd;
        ctx->stop = stop;
        ctx->options = &options;
        ctx->choose = choose_size;
        ctx->user = user;
        ctx->png =
            png_create_read_struct(PNG_LIBPNG_VER_STRING, ctx.get(), on_png_error, on_png_warning);
        if (ctx->png == nullptr) {
            return fail(ErrorCode::OutOfMemory, "png: cannot create the decoder");
        }
        ctx->info = png_create_info_struct(ctx->png);
        if (ctx->info == nullptr) {
            return fail(ErrorCode::OutOfMemory, "png: cannot create the info struct");
        }
        const int rc = run_png_read(*ctx);
        if (ctx->cancelled || stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        if (ctx->io_error) {
            return fail(ErrorCode::IoError,
                        std::string("png: read: ") + std::strerror(ctx->read_errno));
        }
        if (rc != 0) {
            if (ctx->oom || ctx->limit_exceeded) {
                return fail(ErrorCode::OutOfMemory, "png: " + ctx->message);
            }
            if (!ctx->scaler || ctx->rows_done == 0) {
                return fail(ErrorCode::InvalidArgument, "png: " + ctx->message);
            }
            ctx->result.partial = true;
        }
        if (std::optional<RowScaler>& scaler = ctx->scaler; scaler.has_value()) {
            ctx->result.image = scaler->finish();
        }
        return std::move(ctx->result);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "png: allocation failed");
    }
}

Result<void> png_encode_fd(int fd, const ImageBuffer& premultiplied,
                           const std::vector<std::pair<std::string, std::string>>& text) {
    if (!premultiplied.valid()) {
        return fail(ErrorCode::InvalidArgument, "png: invalid image");
    }
    try {
        auto ctx = std::make_unique<PngWriteCtx>();
        ctx->fd = fd;
        ctx->row.resize(std::size_t{premultiplied.width} * 4U);
        for (const auto& [key, value] : text) {
            png_text t{};
            t.compression = PNG_TEXT_COMPRESSION_NONE;
            t.key = const_cast<char*>(key.c_str());
            t.text = const_cast<char*>(value.c_str());
            t.text_length = value.size();
            ctx->texts.push_back(t);
        }
        ctx->png = png_create_write_struct(PNG_LIBPNG_VER_STRING, ctx.get(), on_png_write_error,
                                           on_png_warning);
        if (ctx->png == nullptr) {
            return fail(ErrorCode::OutOfMemory, "png: cannot create the encoder");
        }
        ctx->info = png_create_info_struct(ctx->png);
        if (ctx->info == nullptr) {
            return fail(ErrorCode::OutOfMemory, "png: cannot create the info struct");
        }
        if (run_png_write(*ctx, premultiplied) != 0) {
            if (ctx->io_error) {
                return fail(ErrorCode::IoError,
                            std::string("png: write: ") + std::strerror(ctx->write_errno));
            }
            return fail(ErrorCode::IoError, "png: encode failed: " + ctx->message);
        }
        return ok();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "png: allocation failed");
    }
}

// =============================================================================================
// Image provider entry
// =============================================================================================

namespace {

struct PngChooseState {
    PixelSize target;
    ExifOrientation orientation = ExifOrientation::Normal;
    std::optional<ExifInfo> exif;
};

PixelSize choose_png_size(const PngHeader& h, void* user) {
    auto* st = static_cast<PngChooseState*>(user);
    if (!h.exif.empty()) {
        st->exif = parse_exif(h.exif.data(), h.exif.size());
        if (st->exif) {
            st->orientation = st->exif->orientation;
        }
    }
    if (!check_source_size(h.size, "png")) {
        return {};
    }
    const PixelSize display =
        output_display_size(oriented_size(h.size, st->orientation), st->target);
    return oriented_size(display, st->orientation);
}

} // namespace

Result<ImagePreview> decode_png_file(const Path& path, const DecodeParams& p) {
    auto fd = open_regular(path, nullptr);
    if (!fd) {
        return Unexpected<Error>(fd.error());
    }
    PngDecodeOptions options;
    options.full_decode_pixels = full_decode_limit(p);
    PngChooseState state;
    state.target = p.target;
    auto decoded = png_decode_fd(fd->get(), options, choose_png_size, &state, p.stop);
    if (!decoded) {
        Error e = decoded.error();
        e.message += ": " + path.native();
        return Unexpected<Error>(std::move(e));
    }
    if (decoded->image.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "png: declared size exceeds the limits: " + path.native());
    }
    const PngHeader& h = decoded->header;
    ImagePreview out;
    out.info.format = ImageFormat::Png;
    out.info.stored = h.size;
    out.info.orientation = state.orientation;
    out.info.display = oriented_size(h.size, state.orientation);
    out.info.bits_per_channel = h.bit_depth;
    out.info.has_alpha = h.has_alpha;
    out.info.frame_count = h.frames;
    out.info.animated = h.frames > 1;
    out.info.has_icc_profile = h.has_icc;
    out.info.exif = std::move(state.exif);
    auto oriented = apply_exif_orientation(std::move(decoded->image), state.orientation);
    if (!oriented) {
        return Unexpected<Error>(oriented.error());
    }
    out.image = std::move(*oriented);
    return out;
}

} // namespace navigator::preview::detail
