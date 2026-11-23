// src/preview/decode_bmp.cpp: in-tree BMP decoder. Core (12), info (40), v2/v3 (52/56), v4 (108)
// and v5 (124) headers; 1/4/8-bit palette, 16/24/32-bit RGB and BITFIELDS / ALPHABITFIELDS.
// RLE and embedded JPEG/PNG are Unsupported. Rows are read in blocks with pread (bottom-up files
// read backwards) and streamed into the RowScaler; rows beyond the end of a truncated file are
// transparent.
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <new>

#include "internal.hpp"

namespace navigator::preview::detail {

namespace {

constexpr std::size_t kBlockBytes = std::size_t{1} << 20U;

[[nodiscard]] std::uint32_t le16(const std::uint8_t* p) noexcept {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8U);
}
[[nodiscard]] std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8U) | (std::uint32_t{p[2]} << 16U) |
           (std::uint32_t{p[3]} << 24U);
}

struct Channel {
    std::uint32_t mask = 0;
    unsigned shift = 0;
    unsigned bits = 0;

    explicit Channel(std::uint32_t m = 0) : mask(m) {
        if (m != 0) {
            shift = static_cast<unsigned>(std::countr_zero(m));
            bits = static_cast<unsigned>(std::popcount(m >> shift));
        }
    }
    [[nodiscard]] std::uint8_t get(std::uint32_t v) const noexcept {
        if (mask == 0) {
            return 0;
        }
        const std::uint32_t raw = (v & mask) >> shift;
        if (bits >= 8) {
            return static_cast<std::uint8_t>(raw >> (bits - 8U));
        }
        const std::uint32_t max = (1U << bits) - 1U;
        return static_cast<std::uint8_t>((raw * 255U + max / 2U) / max);
    }
};

} // namespace

Result<ImagePreview> decode_bmp_file(const Path& path, const DecodeParams& p) {
    std::uint64_t file_size = 0;
    auto fd = open_regular(path, &file_size);
    if (!fd) {
        return Unexpected<Error>(fd.error());
    }
    const auto bad = [&](std::string_view what) {
        return fail(ErrorCode::InvalidArgument, "bmp: " + std::string(what) + ": " + path.native());
    };
    std::array<std::uint8_t, 14 + 124> hdr{};
    const long long got = pread_full(fd->get(), hdr.data(), hdr.size(), 0);
    if (got < 0) {
        return Unexpected<Error>(errno_error(errno, "read", path));
    }
    if (got < 26 || hdr[0] != 'B' || hdr[1] != 'M') {
        return bad("not a BMP file");
    }
    const std::uint32_t data_offset = le32(hdr.data() + 10);
    const std::uint32_t dib = le32(hdr.data() + 14);
    if (dib != 12 && dib != 40 && dib != 52 && dib != 56 && dib != 64 && dib != 108 && dib != 124) {
        return bad("unknown header size");
    }
    if (static_cast<std::uint64_t>(got) < 14U + dib) {
        return bad("truncated header");
    }
    const std::uint8_t* d = hdr.data() + 14;
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::uint32_t bpp = 0;
    std::uint32_t compression = 0;
    std::uint32_t colors_used = 0;
    if (dib == 12) {
        width = le16(d + 4);
        height = static_cast<std::int16_t>(le16(d + 6));
        bpp = le16(d + 10);
    } else {
        width = static_cast<std::int32_t>(le32(d + 4));
        height = static_cast<std::int32_t>(le32(d + 8));
        bpp = le16(d + 14);
        compression = le32(d + 16);
        colors_used = le32(d + 32);
    }
    if (compression == 1 || compression == 2 || compression == 4 || compression == 5) {
        return fail(ErrorCode::Unsupported, "bmp: compressed BMP not supported: " + path.native());
    }
    if (compression != 0 && compression != 3 && compression != 6) {
        return bad("unknown compression");
    }
    const bool top_down = height < 0;
    if (height < 0) {
        height = -height;
    }
    if (width <= 0 || height <= 0 || width > kMaxSourceDimension || height > kMaxSourceDimension) {
        return bad("invalid dimensions");
    }
    const PixelSize stored{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    if (auto ok = check_source_size(stored, "bmp"); !ok) {
        return Unexpected<Error>(ok.error());
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        return bad("unsupported bit depth");
    }
    // Masks.
    std::uint32_t rmask = 0;
    std::uint32_t gmask = 0;
    std::uint32_t bmask = 0;
    std::uint32_t amask = 0;
    if (bpp == 16) {
        rmask = 0x7C00;
        gmask = 0x03E0;
        bmask = 0x001F;
    } else if (bpp == 24 || bpp == 32) {
        rmask = 0x00FF0000;
        gmask = 0x0000FF00;
        bmask = 0x000000FF;
    }
    if (compression == 3 || compression == 6) {
        if (bpp != 16 && bpp != 32) {
            return bad("bitfields need 16 or 32 bits");
        }
        std::array<std::uint8_t, 16> masks{};
        if (dib >= 52) {
            std::memcpy(masks.data(), d + 40, 16);
        } else {
            if (pread_full(fd->get(), masks.data(), compression == 6 ? 16 : 12, 14U + dib) < 12) {
                return bad("truncated masks");
            }
        }
        rmask = le32(masks.data());
        gmask = le32(masks.data() + 4);
        bmask = le32(masks.data() + 8);
        amask = (compression == 6 || dib >= 56) ? le32(masks.data() + 12) : 0U;
    } else if (bpp == 32 && dib >= 56) {
        // BI_RGB 32-bit: alpha is not defined; treat as opaque.
        amask = 0;
    }
    const Channel cr(rmask);
    const Channel cg(gmask);
    const Channel cb(bmask);
    const Channel ca(amask);

    // Palette.
    std::array<std::uint8_t, std::size_t{256} * 4U> palette{};
    std::uint32_t palette_count = 0;
    if (bpp <= 8) {
        const std::uint32_t max_colors = 1U << bpp;
        palette_count = colors_used == 0 || colors_used > max_colors ? max_colors : colors_used;
        const std::uint32_t entry = dib == 12 ? 3U : 4U;
        std::array<std::uint8_t, std::size_t{256} * 4U> raw{};
        const long long n =
            pread_full(fd->get(), raw.data(), std::size_t{palette_count} * entry, 14U + dib);
        if (n < 0) {
            return Unexpected<Error>(errno_error(errno, "read", path));
        }
        palette_count = std::min(palette_count, static_cast<std::uint32_t>(n) / entry);
        if (palette_count == 0) {
            return bad("missing palette");
        }
        for (std::uint32_t i = 0; i < palette_count; ++i) {
            palette[std::size_t{i} * 4U] = raw[std::size_t{i} * entry + 2U];
            palette[std::size_t{i} * 4U + 1U] = raw[std::size_t{i} * entry + 1U];
            palette[std::size_t{i} * 4U + 2U] = raw[std::size_t{i} * entry];
            palette[std::size_t{i} * 4U + 3U] = 255;
        }
    }
    const std::uint64_t stride = ((std::uint64_t{stored.width} * bpp + 31U) / 32U) * 4U;
    if (data_offset < 14U + dib || data_offset >= file_size) {
        return bad("invalid pixel data offset");
    }

    try {
        ImagePreview out;
        out.info.format = ImageFormat::Bmp;
        out.info.stored = stored;
        out.info.display = stored;
        out.info.has_alpha = amask != 0;
        out.info.bits_per_channel = 8;
        const PixelSize out_size = output_display_size(stored, p.target);
        auto scaler = RowScaler::create(stored, out_size, false);
        if (!scaler) {
            return Unexpected<Error>(scaler.error());
        }
        const std::uint64_t rows_per_block = std::max<std::uint64_t>(1, kBlockBytes / stride);
        std::vector<std::uint8_t> block(static_cast<std::size_t>(rows_per_block * stride));
        std::vector<std::uint8_t> rgba(std::size_t{stored.width} * 4U);
        std::uint64_t block_first = ~std::uint64_t{0}; // file row index of block[0]
        std::uint64_t block_rows = 0;
        std::uint32_t rows_ok = 0;
        const std::uint64_t rows_in_file = (file_size - data_offset) / stride;
        for (std::uint32_t y = 0; y < stored.height; ++y) {
            if ((y & 63U) == 0U && p.stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            const std::uint64_t file_row = top_down ? y : stored.height - 1U - y;
            if (file_row >= rows_in_file) {
                scaler->push_blank_rows(1);
                continue;
            }
            if (block_first == ~std::uint64_t{0} || file_row < block_first ||
                file_row >= block_first + block_rows) {
                // Load the block that contains file_row (aligned so the next rows hit it).
                std::uint64_t first = file_row;
                if (!top_down) {
                    first = file_row + 1U >= rows_per_block ? file_row + 1U - rows_per_block : 0U;
                }
                const std::uint64_t offset = data_offset + first * stride;
                const long long n = offset >= file_size
                                        ? 0
                                        : pread_full(fd->get(), block.data(), block.size(), offset);
                if (n < 0) {
                    return Unexpected<Error>(errno_error(errno, "read", path));
                }
                block_first = first;
                block_rows = static_cast<std::uint64_t>(n) / stride;
            }
            if (file_row < block_first || file_row >= block_first + block_rows) {
                scaler->push_blank_rows(1);
                continue;
            }
            const std::uint8_t* src = block.data() + (file_row - block_first) * stride;
            for (std::uint32_t x = 0; x < stored.width; ++x) {
                std::uint8_t* px = rgba.data() + std::size_t{x} * 4U;
                if (bpp <= 8) {
                    const std::uint32_t bitpos = x * bpp;
                    const std::uint8_t byte = src[bitpos / 8U];
                    const unsigned shift = 8U - bpp - (bitpos % 8U);
                    const std::uint32_t idx = (byte >> shift) & ((1U << bpp) - 1U);
                    if (idx < palette_count) {
                        std::memcpy(px, palette.data() + std::size_t{idx} * 4U, 4);
                    } else {
                        px[0] = px[1] = px[2] = 0;
                        px[3] = 255;
                    }
                    continue;
                }
                std::uint32_t v = 0;
                if (bpp == 16) {
                    v = le16(src + std::size_t{x} * 2U);
                } else if (bpp == 24) {
                    const std::uint8_t* s = src + std::size_t{x} * 3U;
                    v = std::uint32_t{s[0]} | (std::uint32_t{s[1]} << 8U) |
                        (std::uint32_t{s[2]} << 16U);
                } else {
                    v = le32(src + std::size_t{x} * 4U);
                }
                px[0] = cr.get(v);
                px[1] = cg.get(v);
                px[2] = cb.get(v);
                px[3] = amask != 0 ? ca.get(v) : std::uint8_t{255};
            }
            scaler->push_row(rgba.data());
            ++rows_ok;
        }
        if (rows_ok == 0) {
            return bad("no pixel data");
        }
        out.image = scaler->finish();
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "bmp: allocation failed");
    }
}

} // namespace navigator::preview::detail
