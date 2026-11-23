// src/preview/decode_heif.cpp: HEIC / AVIF through libheif (optional build, NAVIGATOR_HAVE_HEIF).
// Kept within the libheif 1.17 API (Ubuntu 24.04 CI). libheif decodes whole images, so sources
// above the full-decode limit use an embedded thumbnail (Thumbnail purpose) or are refused with
// OutOfMemory. libheif applies irot / imir itself; EXIF orientation is reported as Normal.
#include "internal.hpp"

#if defined(NAVIGATOR_HAVE_HEIF)
#include <cstring>
#include <memory>
#include <new>

#include <libheif/heif.h>
#endif

namespace navigator::preview::detail {

#if defined(NAVIGATOR_HAVE_HEIF)

namespace {

struct HeifLib {
    HeifLib() { (void)heif_init(nullptr); }
    ~HeifLib() { heif_deinit(); }
    HeifLib(const HeifLib&) = delete;
    HeifLib& operator=(const HeifLib&) = delete;
    HeifLib(HeifLib&&) = delete;
    HeifLib& operator=(HeifLib&&) = delete;
};

const HeifLib& heif_lib() {
    static const HeifLib lib;
    return lib;
}

struct CtxDeleter {
    void operator()(heif_context* c) const noexcept { heif_context_free(c); }
};
struct HandleDeleter {
    void operator()(heif_image_handle* h) const noexcept { heif_image_handle_release(h); }
};
struct ImageDeleter {
    void operator()(heif_image* i) const noexcept { heif_image_release(i); }
};
struct OptionsDeleter {
    void operator()(heif_decoding_options* o) const noexcept { heif_decoding_options_free(o); }
};

using HandlePtr = std::unique_ptr<heif_image_handle, HandleDeleter>;

[[nodiscard]] ErrorCode map_heif(const heif_error& e) noexcept {
    switch (e.code) {
    case heif_error_Ok: return ErrorCode::Unknown;
    case heif_error_Memory_allocation_error: return ErrorCode::OutOfMemory;
    case heif_error_Unsupported_feature:
    case heif_error_Unsupported_filetype:
    case heif_error_Plugin_loading_error: return ErrorCode::Unsupported;
    default: return ErrorCode::InvalidArgument;
    }
}

[[nodiscard]] std::optional<ExifInfo> heif_exif(const heif_image_handle* h) {
    heif_item_id ids[4] = {};
    const int n = heif_image_handle_get_list_of_metadata_block_IDs(h, "Exif", ids, 4);
    if (n <= 0) {
        return std::nullopt;
    }
    const std::size_t size = heif_image_handle_get_metadata_size(h, ids[0]);
    if (size < 8 || size > (std::size_t{4} << 20U)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> buf(size);
    if (heif_image_handle_get_metadata(h, ids[0], buf.data()).code != heif_error_Ok) {
        return std::nullopt;
    }
    // 4-byte big-endian offset to the TIFF header, then (usually) "Exif\0\0".
    const std::uint32_t off = (std::uint32_t{buf[0]} << 24U) | (std::uint32_t{buf[1]} << 16U) |
                              (std::uint32_t{buf[2]} << 8U) | std::uint32_t{buf[3]};
    std::size_t start = 4;
    if (off <= size - 4U) {
        start = 4U + off;
    }
    for (std::size_t s : {start, std::size_t{4}}) {
        if (s < size) {
            if (auto info = parse_exif(buf.data() + s, size - s)) {
                return info;
            }
        }
    }
    return std::nullopt;
}

} // namespace

bool heif_has_hevc() noexcept {
    (void)heif_lib();
    return heif_have_decoder_for_format(heif_compression_HEVC) != 0;
}

bool heif_has_av1() noexcept {
    (void)heif_lib();
    return heif_have_decoder_for_format(heif_compression_AV1) != 0;
}

Result<ImagePreview> decode_heif_file(const Path& path, const DecodeParams& p) {
    (void)heif_lib();
    std::uint64_t file_size = 0;
    {
        auto fd = open_regular(path, &file_size);
        if (!fd) {
            return Unexpected<Error>(fd.error());
        }
    }
    if (file_size > p.memory_budget) {
        return fail(ErrorCode::OutOfMemory, "heif: file too large: " + path.native());
    }
    try {
        std::unique_ptr<heif_context, CtxDeleter> ctx(heif_context_alloc());
        if (!ctx) {
            return fail(ErrorCode::OutOfMemory, "heif: cannot create a context");
        }
        heif_context_set_maximum_image_size_limit(ctx.get(), static_cast<int>(kMaxSourceDimension));
        heif_error err = heif_context_read_from_file(ctx.get(), path.c_str(), nullptr);
        if (err.code != heif_error_Ok) {
            return fail(map_heif(err), std::string("heif: ") + (err.message ? err.message : "") +
                                           ": " + path.native());
        }
        if (p.stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        heif_image_handle* raw = nullptr;
        err = heif_context_get_primary_image_handle(ctx.get(), &raw);
        if (err.code != heif_error_Ok || raw == nullptr) {
            return fail(ErrorCode::InvalidArgument, "heif: no primary image: " + path.native());
        }
        HandlePtr primary(raw);
        const PixelSize display{
            static_cast<std::uint32_t>(std::max(heif_image_handle_get_width(primary.get()), 0)),
            static_cast<std::uint32_t>(std::max(heif_image_handle_get_height(primary.get()), 0))};
        if (auto ok = check_source_size(display, "heif"); !ok) {
            return Unexpected<Error>(ok.error());
        }
        ImagePreview out;
        out.info.format =
            sniff_file(path) == ImageFormat::Avif ? ImageFormat::Avif : ImageFormat::Heif;
        out.info.stored = display;
        out.info.display = display;
        out.info.has_alpha = heif_image_handle_has_alpha_channel(primary.get()) != 0;
        const int bits = heif_image_handle_get_luma_bits_per_pixel(primary.get());
        out.info.bits_per_channel = static_cast<std::uint8_t>(bits > 0 && bits <= 16 ? bits : 8);
        out.info.has_icc_profile = heif_image_handle_get_color_profile_type(primary.get()) !=
                                   heif_color_profile_type_not_present;
        out.info.exif = heif_exif(primary.get());
        out.info.frame_count = 1;

        // Pick what to decode: the primary image, or an embedded thumbnail when the primary is
        // too large to decode whole.
        HandlePtr thumb;
        const heif_image_handle* to_decode = primary.get();
        if (display.pixels() > full_decode_limit(p)) {
            if (p.purpose == PreviewPurpose::Thumbnail &&
                heif_image_handle_get_number_of_thumbnails(primary.get()) > 0) {
                heif_item_id tid = 0;
                if (heif_image_handle_get_list_of_thumbnail_IDs(primary.get(), &tid, 1) == 1) {
                    heif_image_handle* th = nullptr;
                    if (heif_image_handle_get_thumbnail(primary.get(), tid, &th).code ==
                            heif_error_Ok &&
                        th != nullptr) {
                        thumb.reset(th);
                    }
                }
            }
            if (!thumb) {
                return fail(ErrorCode::OutOfMemory,
                            "heif: image too large to decode whole: " + path.native());
            }
            const std::uint64_t tp =
                std::uint64_t(std::max(heif_image_handle_get_width(thumb.get()), 0)) *
                std::uint64_t(std::max(heif_image_handle_get_height(thumb.get()), 0));
            if (tp == 0 || tp > full_decode_limit(p)) {
                return fail(ErrorCode::OutOfMemory, "heif: thumbnail too large: " + path.native());
            }
            to_decode = thumb.get();
            out.origin = ImageOrigin::EmbeddedThumbnail;
        }
        std::unique_ptr<heif_decoding_options, OptionsDeleter> options(
            heif_decoding_options_alloc());
        heif_image* img_raw = nullptr;
        err = heif_decode_image(to_decode, &img_raw, heif_colorspace_RGB,
                                heif_chroma_interleaved_RGBA, options.get());
        std::unique_ptr<heif_image, ImageDeleter> img(img_raw);
        if (err.code != heif_error_Ok || !img) {
            return fail(map_heif(err), std::string("heif: decode: ") +
                                           (err.message ? err.message : "") + ": " + path.native());
        }
        if (p.stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        int stride = 0;
        const std::uint8_t* plane =
            heif_image_get_plane_readonly(img.get(), heif_channel_interleaved, &stride);
        const int w = heif_image_get_width(img.get(), heif_channel_interleaved);
        const int h = heif_image_get_height(img.get(), heif_channel_interleaved);
        if (plane == nullptr || w <= 0 || h <= 0 || stride < w * 4 ||
            heif_image_get_bits_per_pixel(img.get(), heif_channel_interleaved) != 32) {
            return fail(ErrorCode::InvalidArgument,
                        "heif: unexpected decoded layout: " + path.native());
        }
        const PixelSize decoded{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
        // Fit the (possibly thumbnail) pixels into the box the primary would get.
        PixelSize box = output_display_size(display, p.target);
        box.width = std::min(box.width, decoded.width);
        box.height = std::min(box.height, decoded.height);
        auto scaled = scale_and_orient(plane, decoded, static_cast<std::size_t>(stride),
                                       heif_image_is_premultiplied_alpha(img.get()) != 0,
                                       ExifOrientation::Normal, box);
        if (!scaled) {
            return Unexpected<Error>(scaled.error());
        }
        out.image = std::move(*scaled);
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "heif: allocation failed");
    }
}

#else // !NAVIGATOR_HAVE_HEIF

bool heif_has_hevc() noexcept { return false; }
bool heif_has_av1() noexcept { return false; }

Result<ImagePreview> decode_heif_file(const Path& path, const DecodeParams& /*p*/) {
    return fail(ErrorCode::Unsupported, "heif: built without libheif: " + path.native());
}

#endif

} // namespace navigator::preview::detail
