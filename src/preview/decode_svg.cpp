// src/preview/decode_svg.cpp: SVG through librsvg + cairo (optional build, NAVIGATOR_HAVE_RSVG).
// The document is loaded from memory with no base file, so librsvg resolves no external
// references (images, stylesheets). Rendering happens directly at the output size (vector),
// bounded by the full-decode limit. SVG files above 16 MiB are refused.
#include "internal.hpp"

#if defined(NAVIGATOR_HAVE_RSVG)
#include <cmath>
#include <cstring>
#include <new>

#include <cairo.h>
#include <librsvg/rsvg.h>
#endif

namespace navigator::preview::detail {

#if defined(NAVIGATOR_HAVE_RSVG)

namespace {

constexpr std::uint64_t kMaxSvgBytes = std::uint64_t{16} << 20U;
constexpr double kDefaultSvgSide = 512.0;

} // namespace

Result<ImagePreview> decode_svg_file(const Path& path, const DecodeParams& p) {
    std::uint64_t file_size = 0;
    auto fd = open_regular(path, &file_size);
    if (!fd) {
        return Unexpected<Error>(fd.error());
    }
    if (file_size > kMaxSvgBytes) {
        return fail(ErrorCode::OutOfMemory, "svg: file too large: " + path.native());
    }
    auto bytes = read_prefix(fd->get(), kMaxSvgBytes, p.stop, path);
    if (!bytes) {
        return Unexpected<Error>(bytes.error());
    }
    GError* gerr = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_data(bytes->data(), bytes->size(), &gerr);
    if (handle == nullptr) {
        std::string msg =
            gerr != nullptr && gerr->message != nullptr ? gerr->message : "parse error";
        if (gerr != nullptr) {
            g_error_free(gerr);
        }
        return fail(ErrorCode::InvalidArgument, "svg: " + msg + ": " + path.native());
    }
    struct HandleGuard {
        RsvgHandle* h;
        explicit HandleGuard(RsvgHandle* handle_in) : h(handle_in) {}
        HandleGuard(const HandleGuard&) = delete;
        HandleGuard& operator=(const HandleGuard&) = delete;
        HandleGuard(HandleGuard&&) = delete;
        HandleGuard& operator=(HandleGuard&&) = delete;
        ~HandleGuard() { g_object_unref(h); }
    } guard(handle);
    rsvg_handle_set_dpi(handle, 96.0);
    double w = 0.0;
    double h = 0.0;
    if (rsvg_handle_get_intrinsic_size_in_pixels(handle, &w, &h) == 0 || !(w > 0.0) || !(h > 0.0)) {
        gboolean has_w = 0;
        gboolean has_h = 0;
        gboolean has_vb = 0;
        RsvgLength lw{};
        RsvgLength lh{};
        RsvgRectangle vb{};
        rsvg_handle_get_intrinsic_dimensions(handle, &has_w, &lw, &has_h, &lh, &has_vb, &vb);
        if (has_vb != 0 && vb.width > 0.0 && vb.height > 0.0) {
            const double k = kDefaultSvgSide / std::max(vb.width, vb.height);
            w = vb.width * k;
            h = vb.height * k;
        } else {
            w = kDefaultSvgSide;
            h = kDefaultSvgSide;
        }
    }
    if (!std::isfinite(w) || !std::isfinite(h)) {
        return fail(ErrorCode::InvalidArgument, "svg: invalid size: " + path.native());
    }
    const auto iw = static_cast<std::uint32_t>(std::clamp(std::ceil(w), 1.0, 65535.0));
    const auto ih = static_cast<std::uint32_t>(std::clamp(std::ceil(h), 1.0, 65535.0));
    const PixelSize intrinsic{iw, ih};
    ImagePreview out;
    out.info.format = ImageFormat::Svg;
    out.info.stored = intrinsic;
    out.info.display = intrinsic;
    out.info.has_alpha = true;
    PixelSize size = fit_size(intrinsic, p.target);
    if (size.pixels() > full_decode_limit(p)) {
        size = fit_size(intrinsic, {4096, 4096});
    }
    if (p.stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "cancelled");
    }
    cairo_surface_t* surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, static_cast<int>(size.width), static_cast<int>(size.height));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return fail(ErrorCode::OutOfMemory, "svg: cannot create a surface");
    }
    cairo_t* cr = cairo_create(surface);
    RsvgRectangle viewport{0.0, 0.0, static_cast<double>(size.width),
                           static_cast<double>(size.height)};
    const gboolean rendered = rsvg_handle_render_document(handle, cr, &viewport, &gerr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    if (rendered == 0) {
        std::string msg =
            gerr != nullptr && gerr->message != nullptr ? gerr->message : "render error";
        if (gerr != nullptr) {
            g_error_free(gerr);
        }
        cairo_surface_destroy(surface);
        return fail(ErrorCode::InvalidArgument, "svg: " + msg + ": " + path.native());
    }
    auto image = make_image(size);
    if (!image) {
        cairo_surface_destroy(surface);
        return Unexpected<Error>(image.error());
    }
    const unsigned char* data = cairo_image_surface_get_data(surface);
    const int cstride = cairo_image_surface_get_stride(surface);
    bool opaque = true;
    for (std::uint32_t y = 0; y < size.height; ++y) {
        const unsigned char* src = data + static_cast<std::size_t>(cstride) * y;
        std::uint8_t* dst = image->pixels.data() + std::size_t{y} * image->stride;
        for (std::uint32_t x = 0; x < size.width; ++x) {
            std::uint32_t v = 0; // native-endian premultiplied ARGB
            std::memcpy(&v, src + std::size_t{x} * 4U, 4);
            dst[std::size_t{x} * 4U] = static_cast<std::uint8_t>(v >> 16U);
            dst[std::size_t{x} * 4U + 1U] = static_cast<std::uint8_t>(v >> 8U);
            dst[std::size_t{x} * 4U + 2U] = static_cast<std::uint8_t>(v);
            dst[std::size_t{x} * 4U + 3U] = static_cast<std::uint8_t>(v >> 24U);
            opaque = opaque && (v >> 24U) == 255U;
        }
    }
    cairo_surface_destroy(surface);
    image->opaque = opaque;
    out.image = std::move(*image);
    return out;
}

#else // !NAVIGATOR_HAVE_RSVG

Result<ImagePreview> decode_svg_file(const Path& path, const DecodeParams& /*p*/) {
    return fail(ErrorCode::Unsupported, "svg: built without librsvg: " + path.native());
}

#endif

} // namespace navigator::preview::detail
