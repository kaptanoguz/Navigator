// src/preview/decode_dispatch.cpp: content sniffing and dispatch to the format decoders.
#include <array>

#include "internal.hpp"

namespace navigator::preview::detail {

ImageFormat sniff_file(const Path& path) noexcept {
    try {
        auto fd = open_regular(path, nullptr);
        if (!fd) {
            return ImageFormat::Unknown;
        }
        std::array<std::uint8_t, 256> head{};
        const long long n = pread_full(fd->get(), head.data(), head.size(), 0);
        if (n <= 0) {
            return ImageFormat::Unknown;
        }
        return sniff_image_format(
            Span<const std::uint8_t>(head.data(), static_cast<std::size_t>(n)));
    } catch (...) {
        return ImageFormat::Unknown;
    }
}

Result<ImagePreview> decode_image_file(const Path& path, const DecodeParams& p, bool nofollow) {
    std::array<std::uint8_t, 256> head{};
    long long n = 0;
    {
        auto fd = open_regular(path, nullptr, nofollow);
        if (!fd) {
            return Unexpected<Error>(fd.error());
        }
        n = pread_full(fd->get(), head.data(), head.size(), 0);
        if (n < 0) {
            return Unexpected<Error>(errno_error(errno, "read", path));
        }
    }
    const ImageFormat format =
        sniff_image_format(Span<const std::uint8_t>(head.data(), static_cast<std::size_t>(n)));
    if (p.stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "cancelled");
    }
    switch (format) {
    case ImageFormat::Jpeg: return decode_jpeg_file(path, p);
    case ImageFormat::Png: return decode_png_file(path, p);
    case ImageFormat::WebP: return decode_webp_file(path, p);
    case ImageFormat::Gif: return decode_gif_file(path, p);
    case ImageFormat::Bmp: return decode_bmp_file(path, p);
    case ImageFormat::Heif:
    case ImageFormat::Avif: return decode_heif_file(path, p);
    case ImageFormat::Svg: return decode_svg_file(path, p);
    case ImageFormat::Unknown:
    case ImageFormat::Tiff:
    case ImageFormat::Ico: break;
    }
    return fail(ErrorCode::Unsupported, "no built-in decoder for format " +
                                            std::string(to_string(format)) + ": " + path.native());
}

} // namespace navigator::preview::detail
