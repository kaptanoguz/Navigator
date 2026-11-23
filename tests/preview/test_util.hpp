// tests/preview/test_util.hpp: temporary fixture directories and tiny image encoders used to
// generate every test fixture at run time (PNG/JPEG/WebP with the same libraries the decoders
// use, GIF/BMP by hand, EXIF blocks by hand). Nothing here touches ~/.cache.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/preview.hpp>

namespace navigator::test {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32U) ^ rd());
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::filesystem::path p =
                base / ("navigator-preview-test-" + std::to_string(gen() & 0xFFFFFFFFFFFFULL));
            if (::mkdir(p.c_str(), 0700) == 0) {
                path_ = p;
                return;
            }
        }
        throw std::runtime_error("TempDir: cannot create a temporary directory");
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(std::string_view name) const {
        return path_ / std::string(name);
    }

private:
    std::filesystem::path path_;
};

inline void write_file(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        throw std::runtime_error("write_file failed: " + p.native());
    }
}

inline void write_text(const std::filesystem::path& p, std::string_view text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

inline std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Straight-alpha RGBA test image.
struct Rgba {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> px;

    Rgba(std::uint32_t w, std::uint32_t h) : width(w), height(h), px(std::size_t{w} * h * 4U, 0) {}
    std::uint8_t* at(std::uint32_t x, std::uint32_t y) {
        return px.data() + (std::size_t{y} * width + x) * 4U;
    }
    void fill_rect(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1,
                   std::array<std::uint8_t, 4> c) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = x0; x < x1; ++x) {
                std::memcpy(at(x, y), c.data(), 4);
            }
        }
    }
};

inline constexpr std::array<std::uint8_t, 4> kRed{255, 0, 0, 255};
inline constexpr std::array<std::uint8_t, 4> kGreen{0, 255, 0, 255};
inline constexpr std::array<std::uint8_t, 4> kBlue{0, 0, 255, 255};
inline constexpr std::array<std::uint8_t, 4> kWhite{255, 255, 255, 255};

// Quadrants TL red, TR green, BL blue, BR white.
inline Rgba quadrants(std::uint32_t w, std::uint32_t h) {
    Rgba img(w, h);
    img.fill_rect(0, 0, w / 2, h / 2, kRed);
    img.fill_rect(w / 2, 0, w, h / 2, kGreen);
    img.fill_rect(0, h / 2, w / 2, h, kBlue);
    img.fill_rect(w / 2, h / 2, w, h, kWhite);
    return img;
}

// Pixel of a premultiplied ImageBuffer.
inline std::array<std::uint8_t, 4> pixel(const preview::ImageBuffer& b, std::uint32_t x,
                                         std::uint32_t y) {
    std::array<std::uint8_t, 4> out{};
    std::memcpy(out.data(), b.pixels.data() + std::size_t{y} * b.stride + std::size_t{x} * 4U, 4);
    return out;
}

inline bool near(std::array<std::uint8_t, 4> a, std::array<std::uint8_t, 4> b, int tol) {
    for (int i = 0; i < 4; ++i) {
        if (std::abs(int{a[static_cast<std::size_t>(i)]} - int{b[static_cast<std::size_t>(i)]}) >
            tol) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Encoders (implemented in test_util.cpp)
// ---------------------------------------------------------------------------------------------

struct PngOptions {
    int bit_depth = 8; // 8 or 16
    int color = 0;     // 0 RGBA, 1 RGB, 2 gray, 3 gray+alpha, 4 palette
    bool interlaced = false;
    std::vector<std::pair<std::string, std::string>> text;
    std::vector<std::uint8_t> exif; // eXIf payload (raw TIFF)
    int compression = 6;
};
std::vector<std::uint8_t> encode_png(const Rgba& img, const PngOptions& opt = {});

struct JpegOptions {
    int quality = 90;
    bool gray = false;
    bool progressive = false;
    std::vector<std::uint8_t> app1; // full APP1 payload ("Exif\0\0" + TIFF)
};
std::vector<std::uint8_t> encode_jpeg(const Rgba& img, const JpegOptions& opt = {});

std::vector<std::uint8_t> encode_webp(const Rgba& img, bool lossless = true);
std::vector<std::uint8_t> encode_gif(const Rgba& img, bool interlaced = false,
                                     int transparent_index = -1, int extra_frames = 0);
std::vector<std::uint8_t> encode_bmp(const Rgba& img, int bpp = 24, bool top_down = false);

// EXIF APP1 ("Exif\0\0" + little-endian TIFF) with orientation and a few Inspector fields.
struct ExifFields {
    std::uint16_t orientation = 1;
    std::string make = "NavCam";
    std::string model = "Model X";
    std::string datetime = "2024:05:06 07:08:09";
    std::string offset = "+03:00";
    std::uint32_t exposure_num = 1;
    std::uint32_t exposure_den = 250;
    std::uint32_t f_num = 28;
    std::uint32_t f_den = 10;
    std::uint16_t iso = 400;
    bool gps = true;
};
std::vector<std::uint8_t> make_exif_app1(const ExifFields& f);

// PNG chunk helpers for crafted files.
std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t crc = 0);
std::vector<std::uint8_t> png_chunk(std::string_view type, const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> png_ihdr_only(std::uint32_t w, std::uint32_t h, int bit_depth = 8,
                                        int color_type = 6);

} // namespace navigator::test
