// tests/preview/test_util.cpp: fixture encoders (see test_util.hpp).
#include "test_util.hpp"

#include <csetjmp>
#include <map>

#include <jpeglib.h>
#include <png.h>
#include <webp/encode.h>

namespace navigator::test {

// ---------------------------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------------------------

namespace {

void png_mem_write(png_structp png, png_bytep data, png_size_t len) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(png_get_io_ptr(png));
    out->insert(out->end(), data, data + len);
}
void png_mem_flush(png_structp /*png*/) {}

} // namespace

std::vector<std::uint8_t> encode_png(const Rgba& img, const PngOptions& opt) {
    std::vector<std::uint8_t> out;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        throw std::runtime_error("encode_png failed");
    }
    png_set_write_fn(png, &out, png_mem_write, png_mem_flush);
    png_set_compression_level(png, opt.compression);
    int color_type = PNG_COLOR_TYPE_RGBA;
    int channels = 4;
    switch (opt.color) {
    case 1:
        color_type = PNG_COLOR_TYPE_RGB;
        channels = 3;
        break;
    case 2:
        color_type = PNG_COLOR_TYPE_GRAY;
        channels = 1;
        break;
    case 3:
        color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
        channels = 2;
        break;
    case 4:
        color_type = PNG_COLOR_TYPE_PALETTE;
        channels = 1;
        break;
    default: break;
    }
    const int depth = opt.color == 4 ? 8 : opt.bit_depth;
    png_set_IHDR(png, info, img.width, img.height, depth, color_type,
                 opt.interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    std::map<std::uint32_t, std::uint8_t> index;
    std::vector<png_color> palette;
    std::vector<png_byte> trns;
    if (opt.color == 4) {
        for (std::size_t i = 0; i < img.px.size(); i += 4) {
            const std::uint32_t key =
                std::uint32_t{img.px[i]} | (std::uint32_t{img.px[i + 1]} << 8U) |
                (std::uint32_t{img.px[i + 2]} << 16U) | (std::uint32_t{img.px[i + 3]} << 24U);
            if (index.find(key) == index.end()) {
                index[key] = static_cast<std::uint8_t>(palette.size());
                palette.push_back(png_color{img.px[i], img.px[i + 1], img.px[i + 2]});
                trns.push_back(img.px[i + 3]);
            }
        }
        png_set_PLTE(png, info, palette.data(), static_cast<int>(palette.size()));
        png_set_tRNS(png, info, trns.data(), static_cast<int>(trns.size()), nullptr);
    }
    std::vector<png_text> texts;
    for (const auto& [k, v] : opt.text) {
        png_text t{};
        t.compression = PNG_TEXT_COMPRESSION_NONE;
        t.key = const_cast<char*>(k.c_str());
        t.text = const_cast<char*>(v.c_str());
        t.text_length = v.size();
        texts.push_back(t);
    }
    if (!texts.empty()) {
        png_set_text(png, info, texts.data(), static_cast<int>(texts.size()));
    }
    if (!opt.exif.empty()) {
        png_set_eXIf_1(png, info, static_cast<png_uint_32>(opt.exif.size()),
                       const_cast<png_bytep>(opt.exif.data()));
    }
    png_write_info(png, info);
    const auto bytes_per = static_cast<std::size_t>(depth / 8);
    std::vector<std::vector<png_byte>> rows(
        img.height, std::vector<png_byte>(std::size_t{img.width} *
                                          static_cast<std::size_t>(channels) * bytes_per));
    for (std::uint32_t y = 0; y < img.height; ++y) {
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::uint8_t* s = img.px.data() + (std::size_t{y} * img.width + x) * 4U;
            std::uint8_t vals[4] = {s[0], s[1], s[2], s[3]};
            int n = channels;
            if (opt.color == 2) {
                vals[0] = s[0];
            } else if (opt.color == 3) {
                vals[0] = s[0];
                vals[1] = s[3];
            } else if (opt.color == 4) {
                const std::uint32_t key = std::uint32_t{s[0]} | (std::uint32_t{s[1]} << 8U) |
                                          (std::uint32_t{s[2]} << 16U) |
                                          (std::uint32_t{s[3]} << 24U);
                vals[0] = index[key];
                n = 1;
            }
            png_byte* d = rows[y].data() + std::size_t{x} * static_cast<std::size_t>(n) * bytes_per;
            for (int c = 0; c < n; ++c) {
                if (bytes_per == 2) {
                    const unsigned v16 = vals[c] * 257U;
                    d[static_cast<std::ptrdiff_t>(c) * 2] = static_cast<png_byte>(v16 >> 8U);
                    d[static_cast<std::ptrdiff_t>(c) * 2 + 1] = static_cast<png_byte>(v16 & 0xFFU);
                } else {
                    d[c] = vals[c];
                }
            }
        }
    }
    std::vector<png_bytep> ptrs;
    ptrs.reserve(rows.size());
    for (auto& r : rows) {
        ptrs.push_back(r.data());
    }
    png_write_image(png, ptrs.data());
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return out;
}

// ---------------------------------------------------------------------------------------------
// JPEG
// ---------------------------------------------------------------------------------------------

std::vector<std::uint8_t> encode_jpeg(const Rgba& img, const JpegOptions& opt) {
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char* mem = nullptr;
    unsigned long mem_size = 0;
    jpeg_mem_dest(&cinfo, &mem, &mem_size);
    cinfo.image_width = img.width;
    cinfo.image_height = img.height;
    cinfo.input_components = opt.gray ? 1 : 3;
    cinfo.in_color_space = opt.gray ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, opt.quality, TRUE);
    if (opt.progressive) {
        jpeg_simple_progression(&cinfo);
    }
    jpeg_start_compress(&cinfo, TRUE);
    if (!opt.app1.empty()) {
        jpeg_write_marker(&cinfo, JPEG_APP0 + 1, opt.app1.data(),
                          static_cast<unsigned int>(opt.app1.size()));
    }
    std::vector<std::uint8_t> row(std::size_t{img.width} * 3U);
    while (cinfo.next_scanline < cinfo.image_height) {
        const std::uint32_t y = cinfo.next_scanline;
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::uint8_t* s = img.px.data() + (std::size_t{y} * img.width + x) * 4U;
            if (opt.gray) {
                row[x] = s[0];
            } else {
                row[std::size_t{x} * 3U] = s[0];
                row[std::size_t{x} * 3U + 1U] = s[1];
                row[std::size_t{x} * 3U + 2U] = s[2];
            }
        }
        JSAMPROW r = row.data();
        jpeg_write_scanlines(&cinfo, &r, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::vector<std::uint8_t> out(mem, mem + mem_size);
    std::free(mem);
    return out;
}

// ---------------------------------------------------------------------------------------------
// WebP
// ---------------------------------------------------------------------------------------------

std::vector<std::uint8_t> encode_webp(const Rgba& img, bool lossless) {
    std::uint8_t* out = nullptr;
    const int stride = static_cast<int>(img.width * 4U);
    const std::size_t n = lossless
                              ? WebPEncodeLosslessRGBA(img.px.data(), static_cast<int>(img.width),
                                                       static_cast<int>(img.height), stride, &out)
                              : WebPEncodeRGBA(img.px.data(), static_cast<int>(img.width),
                                               static_cast<int>(img.height), stride, 90.0F, &out);
    if (n == 0 || out == nullptr) {
        throw std::runtime_error("encode_webp failed");
    }
    std::vector<std::uint8_t> v(out, out + n);
    WebPFree(out);
    return v;
}

// ---------------------------------------------------------------------------------------------
// GIF (uncompressed LZW: a clear code before the table would grow the code size)
// ---------------------------------------------------------------------------------------------

namespace {

void put16(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFFU));
    v.push_back(static_cast<std::uint8_t>((x >> 8U) & 0xFFU));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    put16(v, x & 0xFFFFU);
    put16(v, x >> 16U);
}

} // namespace

std::vector<std::uint8_t> encode_gif(const Rgba& img, bool interlaced, int transparent_index,
                                     int extra_frames) {
    std::map<std::uint32_t, std::uint8_t> index;
    std::vector<std::uint8_t> palette;
    std::vector<std::uint8_t> indices;
    for (std::size_t i = 0; i < img.px.size(); i += 4) {
        const std::uint32_t key = std::uint32_t{img.px[i]} | (std::uint32_t{img.px[i + 1]} << 8U) |
                                  (std::uint32_t{img.px[i + 2]} << 16U);
        auto it = index.find(key);
        if (it == index.end()) {
            if (index.size() >= 256) {
                throw std::runtime_error("encode_gif: more than 256 colours");
            }
            it = index.emplace(key, static_cast<std::uint8_t>(index.size())).first;
            palette.insert(palette.end(), {img.px[i], img.px[i + 1], img.px[i + 2]});
        }
        indices.push_back(it->second);
    }
    int bits = 1;
    while ((1U << static_cast<unsigned>(bits)) < index.size()) {
        ++bits;
    }
    const int table_bits = std::max(bits, 1);
    palette.resize((std::size_t{1} << static_cast<unsigned>(table_bits)) * 3U, 0);
    const int min_code = std::max(bits, 2);
    std::vector<std::uint8_t> out = {'G', 'I', 'F', '8', '9', 'a'};
    put16(out, img.width);
    put16(out, img.height);
    out.push_back(static_cast<std::uint8_t>(0x80U | static_cast<unsigned>(table_bits - 1)));
    out.push_back(0);
    out.push_back(0);
    out.insert(out.end(), palette.begin(), palette.end());
    // Row order.
    std::vector<std::uint32_t> rows;
    if (interlaced) {
        constexpr std::uint32_t kStart[] = {0, 4, 2, 1};
        constexpr std::uint32_t kStep[] = {8, 8, 4, 2};
        for (int p = 0; p < 4; ++p) {
            for (std::uint32_t y = kStart[p]; y < img.height; y += kStep[p]) {
                rows.push_back(y);
            }
        }
    } else {
        for (std::uint32_t y = 0; y < img.height; ++y) {
            rows.push_back(y);
        }
    }
    std::vector<std::uint8_t> data;
    {
        const unsigned clear = 1U << static_cast<unsigned>(min_code);
        const unsigned code_size = static_cast<unsigned>(min_code) + 1U;
        std::uint32_t acc = 0;
        unsigned nbits = 0;
        const auto emit = [&](unsigned code) {
            acc |= code << nbits;
            nbits += code_size;
            while (nbits >= 8) {
                data.push_back(static_cast<std::uint8_t>(acc & 0xFFU));
                acc >>= 8U;
                nbits -= 8;
            }
        };
        unsigned since_clear = 0;
        emit(clear);
        for (const std::uint32_t y : rows) {
            for (std::uint32_t x = 0; x < img.width; ++x) {
                if (since_clear == clear - 2U) {
                    emit(clear);
                    since_clear = 0;
                }
                emit(indices[std::size_t{y} * img.width + x]);
                ++since_clear;
            }
        }
        emit(clear + 1U);
        if (nbits > 0) {
            data.push_back(static_cast<std::uint8_t>(acc & 0xFFU));
        }
    }
    for (int frame = 0; frame <= extra_frames; ++frame) {
        if (transparent_index >= 0) {
            out.insert(out.end(), {0x21, 0xF9, 4, 0x01, 0, 0,
                                   static_cast<std::uint8_t>(transparent_index), 0});
        }
        out.push_back(0x2C);
        put16(out, 0);
        put16(out, 0);
        put16(out, img.width);
        put16(out, img.height);
        out.push_back(interlaced ? 0x40 : 0x00);
        out.push_back(static_cast<std::uint8_t>(min_code));
        for (std::size_t i = 0; i < data.size(); i += 255) {
            const std::size_t n = std::min<std::size_t>(255, data.size() - i);
            out.push_back(static_cast<std::uint8_t>(n));
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i),
                       data.begin() + static_cast<std::ptrdiff_t>(i + n));
        }
        out.push_back(0);
    }
    out.push_back(0x3B);
    return out;
}

// ---------------------------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------------------------

std::vector<std::uint8_t> encode_bmp(const Rgba& img, int bpp, bool top_down) {
    const std::uint32_t header = bpp == 32 ? 108U : 40U;
    const std::uint32_t palette_bytes = bpp == 8 ? 256U * 4U : 0U;
    const std::uint32_t stride = ((img.width * static_cast<std::uint32_t>(bpp) + 31U) / 32U) * 4U;
    const std::uint32_t data_off = 14U + header + palette_bytes;
    std::vector<std::uint8_t> out = {'B', 'M'};
    put32(out, data_off + stride * img.height);
    put32(out, 0);
    put32(out, data_off);
    put32(out, header);
    put32(out, img.width);
    const auto h = static_cast<std::int32_t>(img.height);
    put32(out, static_cast<std::uint32_t>(top_down ? -h : h));
    put16(out, 1);
    put16(out, static_cast<std::uint32_t>(bpp));
    put32(out, bpp == 32 ? 3U : 0U);
    put32(out, stride * img.height);
    put32(out, 2835);
    put32(out, 2835);
    put32(out, bpp == 8 ? 256U : 0U);
    put32(out, 0);
    if (bpp == 32) {
        put32(out, 0x00FF0000);
        put32(out, 0x0000FF00);
        put32(out, 0x000000FF);
        put32(out, 0xFF000000);
        put32(out, 0x73524742); // 'sRGB'
        out.resize(14U + header, 0);
    }
    if (bpp == 8) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            out.insert(out.end(), {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i),
                                   static_cast<std::uint8_t>(i), 0});
        }
    }
    for (std::uint32_t r = 0; r < img.height; ++r) {
        const std::uint32_t y = top_down ? r : img.height - 1U - r;
        std::vector<std::uint8_t> row(stride, 0);
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::uint8_t* s = img.px.data() + (std::size_t{y} * img.width + x) * 4U;
            if (bpp == 24) {
                row[std::size_t{x} * 3U] = s[2];
                row[std::size_t{x} * 3U + 1U] = s[1];
                row[std::size_t{x} * 3U + 2U] = s[0];
            } else if (bpp == 32) {
                row[std::size_t{x} * 4U] = s[2];
                row[std::size_t{x} * 4U + 1U] = s[1];
                row[std::size_t{x} * 4U + 2U] = s[0];
                row[std::size_t{x} * 4U + 3U] = s[3];
            } else {
                row[x] = s[0];
            }
        }
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// EXIF
// ---------------------------------------------------------------------------------------------

namespace {

struct Entry {
    std::uint16_t tag;
    std::uint16_t type; // 2 ASCII, 3 SHORT, 4 LONG, 5 RATIONAL
    std::uint32_t count;
    std::vector<std::uint8_t> value;
};

Entry ascii(std::uint16_t tag, const std::string& s) {
    std::vector<std::uint8_t> v(s.begin(), s.end());
    v.push_back(0);
    return {tag, 2, static_cast<std::uint32_t>(v.size()), v};
}
Entry short_(std::uint16_t tag, std::uint16_t x) {
    std::vector<std::uint8_t> v;
    put16(v, x);
    return {tag, 3, 1, v};
}
Entry long_(std::uint16_t tag, std::uint32_t x) {
    std::vector<std::uint8_t> v;
    put32(v, x);
    return {tag, 4, 1, v};
}
Entry rationals(std::uint16_t tag, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& r) {
    std::vector<std::uint8_t> v;
    for (auto [n, d] : r) {
        put32(v, n);
        put32(v, d);
    }
    return {tag, 5, static_cast<std::uint32_t>(r.size()), v};
}

std::size_t ifd_size(const std::vector<Entry>& e) { return 2 + 12 * e.size() + 4; }

void write_ifd(std::vector<std::uint8_t>& out, const std::vector<Entry>& entries,
               std::vector<std::uint8_t>& data, std::size_t data_base) {
    put16(out, static_cast<std::uint32_t>(entries.size()));
    for (const Entry& e : entries) {
        put16(out, e.tag);
        put16(out, e.type);
        put32(out, e.count);
        if (e.value.size() <= 4) {
            std::vector<std::uint8_t> v = e.value;
            v.resize(4, 0);
            out.insert(out.end(), v.begin(), v.end());
        } else {
            put32(out, static_cast<std::uint32_t>(data_base + data.size()));
            data.insert(data.end(), e.value.begin(), e.value.end());
            if (data.size() % 2 != 0) {
                data.push_back(0);
            }
        }
    }
    put32(out, 0);
}

} // namespace

std::vector<std::uint8_t> make_exif_app1(const ExifFields& f) {
    std::vector<Entry> ifd0 = {ascii(0x010F, f.make), ascii(0x0110, f.model),
                               short_(0x0112, f.orientation), long_(0x8769, 0)};
    if (f.gps) {
        ifd0.push_back(long_(0x8825, 0));
    }
    std::vector<Entry> exif = {rationals(0x829A, {{f.exposure_num, f.exposure_den}}),
                               rationals(0x829D, {{f.f_num, f.f_den}}), short_(0x8827, f.iso),
                               ascii(0x9003, f.datetime), ascii(0x9011, f.offset)};
    std::vector<Entry> gps = {ascii(0x0001, "N"), rationals(0x0002, {{41, 1}, {0, 1}, {0, 1}}),
                              ascii(0x0003, "W"), rationals(0x0004, {{29, 1}, {30, 1}, {0, 1}})};
    const std::size_t off0 = 8;
    const std::size_t off_exif = off0 + ifd_size(ifd0);
    const std::size_t off_gps = off_exif + ifd_size(exif);
    const std::size_t data_base = off_gps + (f.gps ? ifd_size(gps) : 0);
    for (Entry& e : ifd0) {
        if (e.tag == 0x8769) {
            e = long_(0x8769, static_cast<std::uint32_t>(off_exif));
        } else if (e.tag == 0x8825) {
            e = long_(0x8825, static_cast<std::uint32_t>(off_gps));
        }
    }
    std::vector<std::uint8_t> tiff = {'I', 'I', 42, 0};
    put32(tiff, 8);
    std::vector<std::uint8_t> data;
    write_ifd(tiff, ifd0, data, data_base);
    write_ifd(tiff, exif, data, data_base);
    if (f.gps) {
        write_ifd(tiff, gps, data, data_base);
    }
    tiff.insert(tiff.end(), data.begin(), data.end());
    std::vector<std::uint8_t> out = {'E', 'x', 'i', 'f', 0, 0};
    out.insert(out.end(), tiff.begin(), tiff.end());
    return out;
}

// ---------------------------------------------------------------------------------------------
// Crafted PNG
// ---------------------------------------------------------------------------------------------

std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t crc) {
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

std::vector<std::uint8_t> png_chunk(std::string_view type, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    const auto n = static_cast<std::uint32_t>(data.size());
    out.insert(out.end(), {static_cast<std::uint8_t>(n >> 24U), static_cast<std::uint8_t>(n >> 16U),
                           static_cast<std::uint8_t>(n >> 8U), static_cast<std::uint8_t>(n)});
    std::vector<std::uint8_t> body(type.begin(), type.end());
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    const std::uint32_t c = crc32(body.data(), body.size());
    out.insert(out.end(), {static_cast<std::uint8_t>(c >> 24U), static_cast<std::uint8_t>(c >> 16U),
                           static_cast<std::uint8_t>(c >> 8U), static_cast<std::uint8_t>(c)});
    return out;
}

std::vector<std::uint8_t> png_ihdr_only(std::uint32_t w, std::uint32_t h, int bit_depth,
                                        int color_type) {
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    for (const std::uint32_t v : {w, h}) {
        ihdr.insert(ihdr.end(),
                    {static_cast<std::uint8_t>(v >> 24U), static_cast<std::uint8_t>(v >> 16U),
                     static_cast<std::uint8_t>(v >> 8U), static_cast<std::uint8_t>(v)});
    }
    ihdr.insert(ihdr.end(), {static_cast<std::uint8_t>(bit_depth),
                             static_cast<std::uint8_t>(color_type), 0, 0, 0});
    const auto c = png_chunk("IHDR", ihdr);
    out.insert(out.end(), c.begin(), c.end());
    // A tiny (invalid for this size) IDAT: a zlib stream of a few zero bytes.
    const std::vector<std::uint8_t> idat = {0x78, 0x9C, 0x63, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01};
    const auto d = png_chunk("IDAT", idat);
    out.insert(out.end(), d.begin(), d.end());
    const auto e = png_chunk("IEND", {});
    out.insert(out.end(), e.begin(), e.end());
    return out;
}

} // namespace navigator::test
