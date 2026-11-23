// src/preview/image_ops.cpp: ImageBuffer helpers (make_image, fit_size, JPEG scale choice,
// EXIF orientation, box downscale), format sniffing and the streaming RowScaler.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include <navigator/preview.hpp>

#include "internal.hpp"

namespace navigator::preview {

namespace {

[[nodiscard]] bool starts_with(Span<const std::uint8_t> head, std::size_t offset,
                               std::string_view magic) noexcept {
    if (head.size() < offset + magic.size()) {
        return false;
    }
    return std::memcmp(head.data() + offset, magic.data(), magic.size()) == 0;
}

[[nodiscard]] std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8U) | (std::uint32_t{p[2]} << 16U) |
           (std::uint32_t{p[3]} << 24U);
}

[[nodiscard]] std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U) |
           (std::uint32_t{p[2]} << 8U) | std::uint32_t{p[3]};
}

[[nodiscard]] bool brand_in(std::string_view brand,
                            std::initializer_list<std::string_view> list) noexcept {
    return std::find(list.begin(), list.end(), brand) != list.end();
}

[[nodiscard]] ImageFormat sniff_ftyp(Span<const std::uint8_t> head) noexcept {
    if (head.size() < 12 || !starts_with(head, 4, "ftyp")) {
        return ImageFormat::Unknown;
    }
    std::size_t box = be32(head.data());
    if (box < 16 || box > head.size()) {
        box = std::min<std::size_t>(head.size(), box < 16 ? 16 : box);
    }
    std::vector<std::string_view> brands;
    const auto brand_at = [&](std::size_t off) {
        return std::string_view(reinterpret_cast<const char*>(head.data() + off), 4);
    };
    brands.push_back(brand_at(8));
    for (std::size_t off = 16; off + 4 <= box; off += 4) {
        brands.push_back(brand_at(off));
    }
    for (const std::string_view b : brands) {
        if (brand_in(b, {"avif", "avis"})) {
            return ImageFormat::Avif;
        }
    }
    for (const std::string_view b : brands) {
        if (brand_in(b, {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1"})) {
            return ImageFormat::Heif;
        }
    }
    return ImageFormat::Unknown;
}

[[nodiscard]] bool is_space(std::uint8_t c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

std::string_view to_string(ImageFormat format) noexcept {
    switch (format) {
    case ImageFormat::Unknown: return "unknown";
    case ImageFormat::Jpeg: return "jpeg";
    case ImageFormat::Png: return "png";
    case ImageFormat::WebP: return "webp";
    case ImageFormat::Gif: return "gif";
    case ImageFormat::Bmp: return "bmp";
    case ImageFormat::Heif: return "heif";
    case ImageFormat::Avif: return "avif";
    case ImageFormat::Tiff: return "tiff";
    case ImageFormat::Svg: return "svg";
    case ImageFormat::Ico: return "ico";
    }
    return "unknown";
}

ImageFormat sniff_image_format(Span<const std::uint8_t> head) noexcept {
    if (head.size() >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) {
        return ImageFormat::Jpeg;
    }
    if (starts_with(head, 0, "\x89PNG\r\n\x1a\n")) {
        return ImageFormat::Png;
    }
    if (starts_with(head, 0, "RIFF") && starts_with(head, 8, "WEBP")) {
        return ImageFormat::WebP;
    }
    if (starts_with(head, 0, "GIF87a") || starts_with(head, 0, "GIF89a")) {
        return ImageFormat::Gif;
    }
    if (starts_with(head, 0, "BM") && head.size() >= 18) {
        const std::uint32_t dib = le32(head.data() + 14);
        if (dib == 12 || dib == 16 || dib == 40 || dib == 52 || dib == 56 || dib == 64 ||
            dib == 108 || dib == 124) {
            return ImageFormat::Bmp;
        }
    }
    if (const ImageFormat f = sniff_ftyp(head); f != ImageFormat::Unknown) {
        return f;
    }
    if (starts_with(head, 0, std::string_view("II*\0", 4)) ||
        starts_with(head, 0, std::string_view("MM\0*", 4))) {
        return ImageFormat::Tiff;
    }
    if (starts_with(head, 0, std::string_view("\0\0\1\0", 4))) {
        return ImageFormat::Ico;
    }
    std::size_t i = 0;
    if (starts_with(head, 0, "\xEF\xBB\xBF")) {
        i = 3;
    }
    while (i < head.size() && is_space(head[i])) {
        ++i;
    }
    if (starts_with(head, i, "<svg")) {
        return ImageFormat::Svg;
    }
    if (starts_with(head, i, "<?xml")) {
        const std::string_view rest(reinterpret_cast<const char*>(head.data() + i),
                                    head.size() - i);
        if (rest.find("<svg") != std::string_view::npos) {
            return ImageFormat::Svg;
        }
    }
    return ImageFormat::Unknown;
}

Result<ImageBuffer> make_image(PixelSize size) {
    if (size.empty() || size.pixels() > kMaxOutputPixels) {
        return fail(ErrorCode::InvalidArgument, "make_image: empty or too large size");
    }
    ImageBuffer image;
    try {
        image.pixels.assign(static_cast<std::size_t>(size.pixels() * 4U), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "make_image: allocation failed");
    }
    image.width = size.width;
    image.height = size.height;
    image.stride = size.width * 4U;
    return image;
}

namespace {

[[nodiscard]] PixelSize cap_output(PixelSize s) noexcept {
    if (s.pixels() <= kMaxOutputPixels) {
        return s;
    }
    const double f =
        std::sqrt(static_cast<double>(kMaxOutputPixels) / static_cast<double>(s.pixels()));
    auto w = static_cast<std::uint32_t>(std::floor(static_cast<double>(s.width) * f));
    auto h = static_cast<std::uint32_t>(std::floor(static_cast<double>(s.height) * f));
    w = std::max<std::uint32_t>(w, 1);
    h = std::max<std::uint32_t>(h, 1);
    while (std::uint64_t{w} * h > kMaxOutputPixels) {
        if (w >= h && w > 1) {
            --w;
        } else if (h > 1) {
            --h;
        } else {
            break;
        }
    }
    return {w, h};
}

} // namespace

PixelSize fit_size(PixelSize source, PixelSize bounds, bool allow_upscale) noexcept {
    if (source.empty()) {
        return {0, 0};
    }
    if (bounds.empty()) {
        return cap_output(source);
    }
    if (!allow_upscale && source.width <= bounds.width && source.height <= bounds.height) {
        return cap_output(source);
    }
    const std::uint64_t sw = source.width;
    const std::uint64_t sh = source.height;
    const std::uint64_t bw = bounds.width;
    const std::uint64_t bh = bounds.height;
    PixelSize out;
    if (sw * bh <= sh * bw) { // height-limited
        out.height = bounds.height;
        const std::uint64_t w = (sw * bh * 2U + sh) / (2U * sh);
        out.width = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(w, 1, bw));
    } else {
        out.width = bounds.width;
        const std::uint64_t h = (sh * bw * 2U + sw) / (2U * sw);
        out.height = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(h, 1, bh));
    }
    return cap_output(out);
}

std::uint32_t select_jpeg_scale_denom(PixelSize stored, PixelSize target) noexcept {
    if (stored.empty()) {
        return 1;
    }
    const auto scaled = [&](std::uint32_t d) {
        return PixelSize{(stored.width + d - 1) / d, (stored.height + d - 1) / d};
    };
    constexpr std::uint32_t kDenoms[] = {1, 2, 4, 8};
    if (target.empty()) {
        for (const std::uint32_t d : kDenoms) {
            if (scaled(d).pixels() <= kMaxOutputPixels) {
                return d;
            }
        }
        return 8;
    }
    std::uint32_t best = 1;
    for (const std::uint32_t d : kDenoms) {
        const PixelSize s = scaled(d);
        if (s.width >= target.width && s.height >= target.height) {
            best = d;
        }
    }
    return best;
}

Result<ImageBuffer> apply_exif_orientation(ImageBuffer image, ExifOrientation orientation) {
    if (!image.valid()) {
        return fail(ErrorCode::InvalidArgument, "apply_exif_orientation: invalid image");
    }
    if (orientation == ExifOrientation::Normal) {
        return image;
    }
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    const PixelSize out_size = oriented_size({w, h}, orientation);
    ImageBuffer out;
    try {
        out.pixels.assign(static_cast<std::size_t>(out_size.pixels() * 4U), 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "apply_exif_orientation: allocation failed");
    }
    out.width = out_size.width;
    out.height = out_size.height;
    out.stride = out_size.width * 4U;
    out.opaque = image.opaque;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* row = image.pixels.data() + std::size_t{y} * image.stride;
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint32_t dx = x;
            std::uint32_t dy = y;
            switch (orientation) {
            case ExifOrientation::Normal: break;
            case ExifOrientation::FlipHorizontal: dx = w - 1 - x; break;
            case ExifOrientation::Rotate180:
                dx = w - 1 - x;
                dy = h - 1 - y;
                break;
            case ExifOrientation::FlipVertical: dy = h - 1 - y; break;
            case ExifOrientation::Transpose:
                dx = y;
                dy = x;
                break;
            case ExifOrientation::Rotate90:
                dx = h - 1 - y;
                dy = x;
                break;
            case ExifOrientation::Transverse:
                dx = h - 1 - y;
                dy = w - 1 - x;
                break;
            case ExifOrientation::Rotate270:
                dx = y;
                dy = w - 1 - x;
                break;
            }
            std::memcpy(out.pixels.data() + std::size_t{dy} * out.stride + std::size_t{dx} * 4U,
                        row + std::size_t{x} * 4U, 4);
        }
    }
    return out;
}

Result<ImageBuffer> downscale(const ImageBuffer& image, PixelSize size) {
    if (!image.valid() || size.empty() || size.width > image.width || size.height > image.height) {
        return fail(ErrorCode::InvalidArgument, "downscale: invalid image or size");
    }
    if (size == image.size()) {
        ImageBuffer copy;
        try {
            copy = image;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "downscale: allocation failed");
        }
        return copy;
    }
    auto scaler = detail::RowScaler::create(image.size(), size, true);
    if (!scaler) {
        return Unexpected<Error>(scaler.error());
    }
    for (std::uint32_t y = 0; y < image.height; ++y) {
        scaler->push_row(image.pixels.data() + std::size_t{y} * image.stride);
    }
    ImageBuffer out = scaler->finish();
    out.opaque = out.opaque && image.opaque;
    return out;
}

// =============================================================================================
// detail
// =============================================================================================

namespace detail {

Result<void> check_source_size(PixelSize stored, std::string_view what) {
    if (stored.empty()) {
        return fail(ErrorCode::InvalidArgument, std::string(what) + ": empty image");
    }
    if (stored.width > kMaxSourceDimension || stored.height > kMaxSourceDimension ||
        stored.pixels() > kMaxSourcePixels) {
        return fail(ErrorCode::InvalidArgument,
                    std::string(what) + ": declared size " + std::to_string(stored.width) + "x" +
                        std::to_string(stored.height) + " exceeds the limits");
    }
    return ok();
}

PixelSize output_display_size(PixelSize display, PixelSize target) noexcept {
    return fit_size(display, target);
}

void premultiply_row(std::uint8_t* rgba, std::size_t pixels) noexcept {
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint8_t* p = rgba + i * 4U;
        const unsigned a = p[3];
        if (a == 255U) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            const unsigned v = p[c] * a + 127U;
            p[c] = static_cast<std::uint8_t>((v + (v >> 8U)) >> 8U);
        }
    }
}

Result<RowScaler> RowScaler::create(PixelSize src, PixelSize dst, bool src_premultiplied) {
    if (src.empty() || dst.empty()) {
        return fail(ErrorCode::InvalidArgument, "RowScaler: empty size");
    }
    dst.width = std::min(dst.width, src.width);
    dst.height = std::min(dst.height, src.height);
    RowScaler s;
    s.src_ = src;
    s.dst_ = dst;
    s.premultiplied_ = src_premultiplied;
    s.identity_x_ = src.width == dst.width;
    auto image = make_image(dst);
    if (!image) {
        return Unexpected<Error>(image.error());
    }
    s.out_ = std::move(*image);
    try {
        s.hrow_.assign(std::size_t{dst.width} * 4U, 0.0F);
        s.acc_.assign(std::size_t{dst.width} * 4U, 0.0F);
        if (!s.identity_x_) {
            s.xmap_.resize(src.width);
            const std::uint64_t sw = src.width;
            const std::uint64_t dw = dst.width;
            const auto inv = static_cast<float>(1.0 / static_cast<double>(sw));
            for (std::uint64_t x = 0; x < sw; ++x) {
                const std::uint64_t start = x * dw;
                const std::uint64_t j0 = start / sw;
                const std::uint64_t end0 = (j0 + 1U) * sw;
                XMap m;
                m.j0 = static_cast<std::uint32_t>(j0);
                if ((x + 1U) * dw > end0 && j0 + 1U < dw) {
                    m.w0 = static_cast<float>(end0 - start) * inv;
                    m.w1 = static_cast<float>((x + 1U) * dw - end0) * inv;
                } else {
                    m.w0 = static_cast<float>(dw) * inv;
                    m.w1 = 0.0F;
                }
                s.xmap_[x] = m;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "RowScaler: allocation failed");
    }
    return s;
}

void RowScaler::emit_row() noexcept {
    if (row_out_ >= dst_.height) {
        return;
    }
    std::uint8_t* out = out_.pixels.data() + std::size_t{row_out_} * out_.stride;
    const std::size_t n = std::size_t{dst_.width} * 4U;
    if (row_weight_ > 0.0) {
        const auto inv = static_cast<float>(1.0 / row_weight_);
        for (std::size_t i = 0; i < n; ++i) {
            const float v = acc_[i] * inv + 0.5F;
            out[i] = v >= 255.0F ? std::uint8_t{255}
                                 : (v <= 0.0F ? std::uint8_t{0} : static_cast<std::uint8_t>(v));
        }
        for (std::size_t i = 3; i < n; i += 4) {
            if (out[i] != 255U) {
                opaque_ = false;
                break;
            }
        }
    } else {
        opaque_ = false;
    }
    std::fill(acc_.begin(), acc_.end(), 0.0F);
    row_weight_ = 0.0;
    ++row_out_;
}

void RowScaler::push_row(const std::uint8_t* rgba) noexcept {
    if (rows_in_ >= src_.height) {
        return;
    }
    const bool identity_y = src_.height == dst_.height;
    if (identity_x_ && identity_y) { // straight copy
        std::uint8_t* out = out_.pixels.data() + std::size_t{rows_in_} * out_.stride;
        std::memcpy(out, rgba, std::size_t{src_.width} * 4U);
        if (!premultiplied_) {
            premultiply_row(out, src_.width);
        }
        for (std::uint32_t x = 0; x < src_.width && opaque_; ++x) {
            if (out[std::size_t{x} * 4U + 3U] != 255U) {
                opaque_ = false;
            }
        }
        ++rows_in_;
        row_out_ = rows_in_;
        return;
    }
    // Horizontal pass into hrow_ (premultiplied, normalized).
    if (identity_x_) {
        for (std::uint32_t x = 0; x < src_.width; ++x) {
            const std::uint8_t* p = rgba + std::size_t{x} * 4U;
            const float a = p[3];
            const float k = premultiplied_ ? 1.0F : a / 255.0F;
            float* h = hrow_.data() + std::size_t{x} * 4U;
            h[0] = static_cast<float>(p[0]) * k;
            h[1] = static_cast<float>(p[1]) * k;
            h[2] = static_cast<float>(p[2]) * k;
            h[3] = a;
        }
    } else {
        std::fill(hrow_.begin(), hrow_.end(), 0.0F);
        for (std::uint32_t x = 0; x < src_.width; ++x) {
            const std::uint8_t* p = rgba + std::size_t{x} * 4U;
            const float a = p[3];
            const float k = premultiplied_ ? 1.0F : a / 255.0F;
            const float v0 = static_cast<float>(p[0]) * k;
            const float v1 = static_cast<float>(p[1]) * k;
            const float v2 = static_cast<float>(p[2]) * k;
            const XMap& m = xmap_[x];
            float* h = hrow_.data() + std::size_t{m.j0} * 4U;
            h[0] += v0 * m.w0;
            h[1] += v1 * m.w0;
            h[2] += v2 * m.w0;
            h[3] += a * m.w0;
            if (m.w1 > 0.0F) {
                h[4] += v0 * m.w1;
                h[5] += v1 * m.w1;
                h[6] += v2 * m.w1;
                h[7] += a * m.w1;
            }
        }
    }
    // Vertical: integer units, source row i spans [i*dh, (i+1)*dh), target r [r*sh, (r+1)*sh).
    const std::uint64_t sh = src_.height;
    const std::uint64_t dh = dst_.height;
    const std::uint64_t start = std::uint64_t{rows_in_} * dh;
    const std::uint64_t end = start + dh;
    const std::uint64_t boundary = (std::uint64_t{row_out_} + 1U) * sh;
    const std::size_t n = hrow_.size();
    if (end <= boundary || row_out_ + 1U >= dst_.height) {
        const auto w = static_cast<float>(dh);
        for (std::size_t i = 0; i < n; ++i) {
            acc_[i] += hrow_[i] * w;
        }
        row_weight_ += static_cast<double>(dh);
        if (end == boundary) {
            emit_row();
        }
    } else {
        const auto w0 = static_cast<float>(boundary - start);
        const auto w1 = static_cast<float>(end - boundary);
        for (std::size_t i = 0; i < n; ++i) {
            acc_[i] += hrow_[i] * w0;
        }
        row_weight_ += static_cast<double>(boundary - start);
        emit_row();
        for (std::size_t i = 0; i < n; ++i) {
            acc_[i] = hrow_[i] * w1;
        }
        row_weight_ = static_cast<double>(end - boundary);
    }
    ++rows_in_;
    if (rows_in_ == src_.height && row_out_ < dst_.height && row_weight_ > 0.0) {
        emit_row();
    }
}

void RowScaler::push_blank_rows(std::uint32_t count) noexcept {
    if (count == 0) {
        return;
    }
    opaque_ = false;
    const std::uint64_t sh = src_.height;
    const std::uint64_t dh = dst_.height;
    for (std::uint32_t k = 0; k < count && rows_in_ < src_.height; ++k) {
        if (src_.height == dst_.height) {
            ++rows_in_;
            row_out_ = rows_in_; // rows stay zero
            continue;
        }
        const std::uint64_t start = std::uint64_t{rows_in_} * dh;
        const std::uint64_t end = start + dh;
        const std::uint64_t boundary = (std::uint64_t{row_out_} + 1U) * sh;
        if (end <= boundary || row_out_ + 1U >= dst_.height) {
            row_weight_ += static_cast<double>(dh);
            if (end == boundary) {
                emit_row();
            }
        } else {
            row_weight_ += static_cast<double>(boundary - start);
            emit_row();
            row_weight_ = static_cast<double>(end - boundary);
        }
        ++rows_in_;
        if (rows_in_ == src_.height && row_out_ < dst_.height && row_weight_ > 0.0) {
            emit_row();
        }
    }
}

ImageBuffer RowScaler::finish() noexcept {
    if (rows_in_ < src_.height) {
        opaque_ = false;
        if (row_weight_ > 0.0 && row_out_ < dst_.height) {
            emit_row();
        }
    }
    out_.opaque = opaque_ && rows_in_ >= src_.height;
    return std::move(out_);
}

Result<ImageBuffer> scale_and_orient(const std::uint8_t* pixels, PixelSize stored,
                                     std::size_t stride, bool premultiplied,
                                     ExifOrientation orientation, PixelSize target) {
    const PixelSize display = output_display_size(oriented_size(stored, orientation), target);
    const PixelSize out_stored = oriented_size(display, orientation); // swap back
    auto scaler = RowScaler::create(stored, out_stored, premultiplied);
    if (!scaler) {
        return Unexpected<Error>(scaler.error());
    }
    for (std::uint32_t y = 0; y < stored.height; ++y) {
        scaler->push_row(pixels + std::size_t{y} * stride);
    }
    return apply_exif_orientation(scaler->finish(), orientation);
}

} // namespace detail

} // namespace navigator::preview
