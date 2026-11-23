// tests/preview/decode_test.cpp: format decoders on generated fixtures: JPEG (DCT scaling, EXIF
// orientations 1-8 and fields, CMYK-free gray, progressive), PNG (8/16-bit, palette, gray,
// interlaced, eXIf), WebP (lossless, lossy, alpha), GIF (first frame, transparency, interlace,
// frame count), BMP (24/32/8-bit, top-down), HEIF and SVG when built, huge declared sizes,
// truncated and corrupt files.
#include <catch2/catch_test_macros.hpp>

#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>

#include "internal.hpp"
#include "test_util.hpp"

using namespace navigator;
using namespace navigator::preview;
namespace pd = navigator::preview::detail;

namespace {

pd::DecodeParams params(PixelSize target = {}, PreviewPurpose purpose = PreviewPurpose::Inspector) {
    pd::DecodeParams p;
    p.target = target;
    p.purpose = purpose;
    return p;
}

Result<ImagePreview> decode(const std::filesystem::path& file, PixelSize target = {}) {
    return pd::decode_image_file(file, params(target));
}

ImagePreview decode_ok(const std::filesystem::path& file, PixelSize target = {}) {
    auto r = decode(file, target);
    INFO((r ? std::string("ok") : r.error().message));
    REQUIRE(r);
    REQUIRE(r->image.valid());
    return std::move(*r);
}

using Color = std::array<std::uint8_t, 4>;

} // namespace

TEST_CASE("JPEG decodes with DCT scaling into the target", "[decode][jpeg]") {
    test::TempDir dir;
    const auto file = dir / "q.jpg";
    test::write_file(file, test::encode_jpeg(test::quadrants(640, 480)));
    const ImagePreview full = decode_ok(file);
    CHECK(full.info.format == ImageFormat::Jpeg);
    CHECK(full.info.stored == PixelSize{640, 480});
    CHECK(full.image.size() == PixelSize{640, 480});
    CHECK(full.image.opaque);
    CHECK(test::near(test::pixel(full.image, 10, 10), test::kRed, 40));
    CHECK(test::near(test::pixel(full.image, 630, 470), test::kWhite, 40));

    const ImagePreview thumb = decode_ok(file, {128, 128});
    CHECK(thumb.image.size() == PixelSize{128, 96});
    CHECK(test::near(test::pixel(thumb.image, 5, 5), test::kRed, 40));
    CHECK(test::near(test::pixel(thumb.image, 120, 5), test::kGreen, 40));
    CHECK(test::near(test::pixel(thumb.image, 5, 90), test::kBlue, 40));
    CHECK_FALSE(full.info.exif.has_value());

    test::JpegOptions gray;
    gray.gray = true;
    gray.progressive = true;
    const auto gfile = dir / "g.jpg";
    test::write_file(gfile, test::encode_jpeg(test::quadrants(64, 64), gray));
    const ImagePreview g = decode_ok(gfile, {32, 32});
    CHECK(g.image.size() == PixelSize{32, 32});
    const Color p = test::pixel(g.image, 2, 2);
    CHECK(p[0] == p[1]);
    CHECK(p[3] == 255);
}

TEST_CASE("JPEG EXIF orientations 1-8 give display pixels and size", "[decode][jpeg][exif]") {
    test::TempDir dir;
    // Stored 64 x 32: TL red, TR green, BL blue, BR white.
    struct Case {
        std::uint16_t o;
        Color tl;
        Color tr;
    };
    const Case cases[] = {
        {1, test::kRed, test::kGreen},   {2, test::kGreen, test::kRed},
        {3, test::kWhite, test::kBlue},  {4, test::kBlue, test::kWhite},
        {5, test::kRed, test::kBlue},    {6, test::kBlue, test::kRed},
        {7, test::kWhite, test::kGreen}, {8, test::kGreen, test::kWhite},
    };
    for (const Case& c : cases) {
        INFO("orientation " << c.o);
        test::ExifFields f;
        f.orientation = c.o;
        test::JpegOptions opt;
        opt.app1 = test::make_exif_app1(f);
        const auto file = dir / ("o" + std::to_string(c.o) + ".jpg");
        test::write_file(file, test::encode_jpeg(test::quadrants(64, 32), opt));
        const ImagePreview img = decode_ok(file);
        CHECK(img.info.orientation == static_cast<ExifOrientation>(c.o));
        CHECK(img.info.stored == PixelSize{64, 32});
        const PixelSize display = c.o >= 5 ? PixelSize{32, 64} : PixelSize{64, 32};
        CHECK(img.info.display == display);
        CHECK(img.image.size() == display);
        CHECK(test::near(test::pixel(img.image, 4, 4), c.tl, 50));
        CHECK(test::near(test::pixel(img.image, img.image.width - 5, 4), c.tr, 50));

        // Thumbnail target is in display orientation.
        auto small = decode(file, {16, 16});
        REQUIRE(small);
        CHECK(small->image.size() == (c.o >= 5 ? PixelSize{8, 16} : PixelSize{16, 8}));
    }
}

TEST_CASE("JPEG EXIF fields are parsed", "[decode][jpeg][exif]") {
    test::TempDir dir;
    test::JpegOptions opt;
    opt.app1 = test::make_exif_app1({});
    const auto file = dir / "e.jpg";
    test::write_file(file, test::encode_jpeg(test::quadrants(32, 32), opt));
    const ImagePreview img = decode_ok(file);
    REQUIRE(img.info.exif);
    const ExifInfo e = img.info.exif.value_or(ExifInfo{});
    CHECK(e.make == "NavCam");
    CHECK(e.model == "Model X");
    CHECK(e.taken.year == 2024);
    CHECK(e.taken.month == 5);
    CHECK(e.taken.second == 9);
    CHECK(e.taken.utc_offset_minutes == std::int16_t{180});
    CHECK(e.exposure_time == Rational{1, 250});
    CHECK(e.f_number.value_or(0.0) > 2.79);
    CHECK(e.f_number.value_or(0.0) < 2.81);
    CHECK(e.iso == 400U);
    CHECK(e.has_gps);
    CHECK(e.gps_latitude.value_or(0.0) > 40.99);
    CHECK(e.gps_longitude.value_or(0.0) < -29.49);
    CHECK(e.gps_longitude.value_or(-99.0) > -29.51);
}

TEST_CASE("clean_exif_string trims, repairs and caps", "[exif]") {
    CHECK(pd::clean_exif_string(std::string_view("  Canon\0\0\0", 10)) == "Canon");
    CHECK(pd::clean_exif_string("a\xFF"
                                "b") == "a\xEF\xBF\xBD"
                                        "b");
    CHECK(pd::clean_exif_string(std::string(1000, 'x')).size() == 256);
    CHECK_FALSE(pd::parse_exif(reinterpret_cast<const std::uint8_t*>("garbage!"), 8));
}

TEST_CASE("PNG variants decode to RGBA8 premultiplied", "[decode][png]") {
    test::TempDir dir;
    test::Rgba img = test::quadrants(40, 30);
    img.fill_rect(0, 0, 4, 4, {255, 0, 0, 128}); // half-transparent red corner
    struct Variant {
        const char* name;
        test::PngOptions opt;
        bool alpha;
    };
    std::vector<Variant> variants;
    variants.push_back({"rgba8", {}, true});
    test::PngOptions o16;
    o16.bit_depth = 16;
    variants.push_back({"rgba16", o16, true});
    test::PngOptions rgb;
    rgb.color = 1;
    variants.push_back({"rgb", rgb, false});
    test::PngOptions gray;
    gray.color = 2;
    variants.push_back({"gray", gray, false});
    test::PngOptions ga16;
    ga16.color = 3;
    ga16.bit_depth = 16;
    variants.push_back({"gray-alpha16", ga16, true});
    test::PngOptions pal;
    pal.color = 4;
    variants.push_back({"palette", pal, true});
    test::PngOptions inter;
    inter.interlaced = true;
    variants.push_back({"interlaced", inter, true});
    for (const Variant& v : variants) {
        INFO(v.name);
        const auto file = dir / (std::string(v.name) + ".png");
        test::write_file(file, test::encode_png(img, v.opt));
        const ImagePreview p = decode_ok(file);
        CHECK(p.info.format == ImageFormat::Png);
        CHECK(p.image.size() == PixelSize{40, 30});
        CHECK(p.info.bits_per_channel == (v.opt.color == 4 ? 8 : v.opt.bit_depth));
        CHECK(p.info.has_alpha == v.alpha);
        const Color br = test::pixel(p.image, 35, 25);
        CHECK(test::near(br, test::kWhite, 2));
        if (v.opt.color != 2 && v.opt.color != 3) {
            CHECK(test::near(test::pixel(p.image, 30, 5), test::kGreen, 2));
        }
        if (v.alpha && v.opt.color != 3) {
            CHECK(test::near(test::pixel(p.image, 1, 1), {128, 0, 0, 128}, 2)); // premultiplied
            CHECK_FALSE(p.image.opaque);
        }
        const ImagePreview t = decode_ok(file, {20, 20});
        CHECK(t.image.size() == PixelSize{20, 15});
    }
}

TEST_CASE("PNG eXIf orientation and tEXt are read", "[decode][png][exif]") {
    test::TempDir dir;
    test::ExifFields f;
    f.orientation = 6;
    auto app1 = test::make_exif_app1(f);
    test::PngOptions opt;
    opt.exif.assign(app1.begin() + 6, app1.end()); // raw TIFF
    const auto file = dir / "x.png";
    test::write_file(file, test::encode_png(test::quadrants(20, 10), opt));
    const ImagePreview p = decode_ok(file);
    CHECK(p.info.orientation == ExifOrientation::Rotate90);
    CHECK(p.image.size() == PixelSize{10, 20});
    CHECK(p.info.exif.value_or(ExifInfo{}).make == "NavCam");
}

TEST_CASE("WebP lossless, lossy and alpha decode", "[decode][webp]") {
    test::TempDir dir;
    test::Rgba img = test::quadrants(50, 40);
    img.fill_rect(0, 0, 10, 10, {0, 0, 0, 0});
    const auto ll = dir / "ll.webp";
    test::write_file(ll, test::encode_webp(img, true));
    const ImagePreview a = decode_ok(ll);
    CHECK(a.info.format == ImageFormat::WebP);
    CHECK(a.info.has_alpha);
    CHECK(a.image.size() == PixelSize{50, 40});
    CHECK(test::pixel(a.image, 2, 2)[3] == 0);
    CHECK(test::near(test::pixel(a.image, 45, 35), test::kWhite, 1));
    const ImagePreview s = decode_ok(ll, {25, 25});
    CHECK(s.image.size() == PixelSize{25, 20});

    const auto lossy = dir / "lossy.webp";
    test::write_file(lossy, test::encode_webp(test::quadrants(64, 64), false));
    const ImagePreview l = decode_ok(lossy, {32, 32});
    CHECK(l.image.size() == PixelSize{32, 32});
    CHECK(test::near(test::pixel(l.image, 28, 28), test::kWhite, 40));
}

TEST_CASE("GIF first frame, transparency, interlace and frame count", "[decode][gif]") {
    test::TempDir dir;
    const test::Rgba img = test::quadrants(30, 20);
    const auto plain = dir / "a.gif";
    test::write_file(plain, test::encode_gif(img));
    const ImagePreview g = decode_ok(plain);
    CHECK(g.info.format == ImageFormat::Gif);
    CHECK(g.image.size() == PixelSize{30, 20});
    CHECK(g.info.frame_count == 1);
    CHECK_FALSE(g.info.animated);
    CHECK(test::pixel(g.image, 2, 2) == test::kRed);
    CHECK(test::pixel(g.image, 28, 18) == test::kWhite);
    CHECK(test::pixel(g.image, 28, 2) == test::kGreen);

    const auto inter = dir / "i.gif";
    test::write_file(inter, test::encode_gif(img, true, -1, 2));
    const ImagePreview i = decode_ok(inter);
    CHECK(test::pixel(i.image, 2, 18) == test::kBlue);
    CHECK(i.info.frame_count == 3);
    CHECK(i.info.animated);

    const auto transp = dir / "t.gif";
    test::write_file(transp, test::encode_gif(img, false, 0)); // index 0 = red
    const ImagePreview t = decode_ok(transp);
    CHECK(t.info.has_alpha);
    CHECK(test::pixel(t.image, 2, 2)[3] == 0);
    CHECK(test::pixel(t.image, 28, 18) == test::kWhite);

    // Larger image so the LZW clear-code path runs many times; 256 colours.
    test::Rgba big(64, 64);
    for (std::uint32_t y = 0; y < 64; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
            const auto v = static_cast<std::uint8_t>((x + y * 64) % 256);
            big.fill_rect(x, y, x + 1, y + 1, {v, static_cast<std::uint8_t>(255 - v), 0, 255});
        }
    }
    const auto bf = dir / "big.gif";
    test::write_file(bf, test::encode_gif(big));
    const ImagePreview b = decode_ok(bf);
    CHECK(test::pixel(b.image, 5, 3) == Color{static_cast<std::uint8_t>((5 + 3 * 64) % 256),
                                              static_cast<std::uint8_t>(255 - (5 + 3 * 64) % 256),
                                              0, 255});
}

TEST_CASE("BMP 24/32/8-bit, bottom-up and top-down", "[decode][bmp]") {
    test::TempDir dir;
    test::Rgba img = test::quadrants(21, 13); // odd width: row padding
    for (const int bpp : {24, 32, 8}) {
        for (const bool top_down : {false, true}) {
            INFO("bpp " << bpp << " top_down " << top_down);
            const auto file = dir / ("b" + std::to_string(bpp) + (top_down ? "t" : "b") + ".bmp");
            test::write_file(file, test::encode_bmp(img, bpp, top_down));
            const ImagePreview b = decode_ok(file);
            CHECK(b.info.format == ImageFormat::Bmp);
            CHECK(b.image.size() == PixelSize{21, 13});
            if (bpp == 8) {
                CHECK(test::pixel(b.image, 1, 1) == Color{255, 255, 255, 255}); // gray from R
                CHECK(test::pixel(b.image, 1, 12) == Color{0, 0, 0, 255});
            } else {
                CHECK(test::pixel(b.image, 1, 1) == test::kRed);
                CHECK(test::pixel(b.image, 20, 0) == test::kGreen);
                CHECK(test::pixel(b.image, 1, 12) == test::kBlue);
            }
        }
    }
    auto bad = test::encode_bmp(img, 24);
    bad[30] = 1; // RLE8 compression
    test::write_file(dir / "rle.bmp", bad);
    CHECK(decode(dir / "rle.bmp").error().code == ErrorCode::Unsupported);
}

TEST_CASE("Huge declared sizes are refused or decoded bounded", "[decode][limits]") {
    test::TempDir dir;
    // PNG: over the side limit, over the pixel limit, and a 100 MP header with no data.
    test::write_file(dir / "wide.png", test::png_ihdr_only(100000, 10));
    CHECK(decode(dir / "wide.png").error().code == ErrorCode::InvalidArgument);
    test::write_file(dir / "gp.png", test::png_ihdr_only(60000, 60000));
    CHECK(decode(dir / "gp.png").error().code == ErrorCode::InvalidArgument);
    test::write_file(dir / "100mp.png", test::png_ihdr_only(10000, 10000));
    {
        auto r = decode(dir / "100mp.png", {256, 256});
        if (r) {
            CHECK(r->image.size() == PixelSize{256, 256});
        } else {
            CHECK(r.error().code == ErrorCode::InvalidArgument);
        }
    }
    // Interlaced 100 MP: cannot stream, refused before allocating the full image.
    test::write_file(dir / "100mp-i.png", [] {
        auto v = test::png_ihdr_only(10000, 10000);
        v[8 + 8 + 12] = 1; // interlace method byte of IHDR (sig 8 + len/type 8 + 12)
        // fix CRC
        const std::uint32_t c = test::crc32(v.data() + 12, 17);
        v[29] = static_cast<std::uint8_t>(c >> 24U);
        v[30] = static_cast<std::uint8_t>(c >> 16U);
        v[31] = static_cast<std::uint8_t>(c >> 8U);
        v[32] = static_cast<std::uint8_t>(c);
        return v;
    }());
    {
        auto r = decode(dir / "100mp-i.png", {256, 256});
        REQUIRE_FALSE(r);
        CHECK((r.error().code == ErrorCode::OutOfMemory ||
               r.error().code == ErrorCode::InvalidArgument));
    }

    // GIF: 65535 x 65535 canvas (over 1 GP) and 30000 x 30000 (over the full-decode limit).
    {
        auto g = test::encode_gif(test::quadrants(4, 4));
        g[6] = g[7] = g[8] = g[9] = 0xFF;
        test::write_file(dir / "huge.gif", g);
        CHECK(decode(dir / "huge.gif").error().code == ErrorCode::InvalidArgument);
        g[6] = g[8] = 0x30;
        g[7] = g[9] = 0x75;
        test::write_file(dir / "big.gif", g);
        CHECK(decode(dir / "big.gif").error().code == ErrorCode::OutOfMemory);
    }
    // BMP: 30000 x 30000 declared, almost no data: bounded partial (blank rows) or error.
    {
        auto b = test::encode_bmp(test::quadrants(4, 4));
        const std::uint32_t side = 30000;
        for (int k = 0; k < 2; ++k) {
            const std::size_t off = 18U + static_cast<std::size_t>(k) * 4U;
            b[off] = static_cast<std::uint8_t>(side);
            b[off + 1] = static_cast<std::uint8_t>(side >> 8U);
            b[off + 2] = 0;
            b[off + 3] = 0;
        }
        test::write_file(dir / "huge.bmp", b);
        auto r = decode(dir / "huge.bmp", {128, 128});
        if (r) {
            CHECK(r->image.size() == PixelSize{128, 128});
        }
    }
    // JPEG: declared 60000 x 60000 (patch the SOF0 dimensions).
    {
        auto j = test::encode_jpeg(test::quadrants(16, 16));
        for (std::size_t i = 0; i + 8 < j.size(); ++i) {
            if (j[i] == 0xFF && j[i + 1] == 0xC0) {
                j[i + 5] = 0xEA;
                j[i + 6] = 0x60;
                j[i + 7] = 0xEA;
                j[i + 8] = 0x60;
                break;
            }
        }
        test::write_file(dir / "huge.jpg", j);
        CHECK(decode(dir / "huge.jpg").error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("A real 100 MP JPEG decodes at 1/8 scale into a thumbnail", "[decode][limits][slow]") {
    test::TempDir dir;
    test::Rgba img(12000, 8400);
    img.fill_rect(0, 0, 12000, 8400, {40, 80, 120, 255});
    test::JpegOptions opt;
    opt.quality = 30;
    const auto file = dir / "100mp.jpg";
    test::write_file(file, test::encode_jpeg(img, opt));
    const ImagePreview t = decode_ok(file, {256, 256});
    CHECK(t.info.stored == PixelSize{12000, 8400});
    CHECK(t.image.size() == PixelSize{256, 179});
    CHECK(test::near(test::pixel(t.image, 100, 100), {40, 80, 120, 255}, 8));
    // Full-resolution request is capped at kMaxOutputPixels.
    const ImagePreview f = decode_ok(file);
    CHECK(f.image.size().pixels() <= kMaxOutputPixels);
    CHECK(f.info.display == PixelSize{12000, 8400});
}

TEST_CASE("Truncated and corrupt files never crash", "[decode][corrupt]") {
    test::TempDir dir;
    const test::Rgba img = test::quadrants(48, 32);
    test::JpegOptions jo;
    jo.app1 = test::make_exif_app1({});
    test::PngOptions inter;
    inter.interlaced = true;
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> fixtures = {
        {"jpg", test::encode_jpeg(img, jo)},      {"png", test::encode_png(img)},
        {"ipng", test::encode_png(img, inter)},   {"webp", test::encode_webp(img, true)},
        {"lwebp", test::encode_webp(img, false)}, {"gif", test::encode_gif(img)},
        {"bmp", test::encode_bmp(img, 24)},
    };
    for (const auto& [name, data] : fixtures) {
        INFO(name);
        for (std::size_t cut = 0; cut < data.size();
             cut += std::max<std::size_t>(1, data.size() / 40)) {
            const auto file = dir / ("t-" + name);
            test::write_file(
                file, std::vector<std::uint8_t>(data.begin(),
                                                data.begin() + static_cast<std::ptrdiff_t>(cut)));
            auto r = decode(file, {24, 24});
            if (r) {
                CHECK(r->image.valid());
            } else {
                CHECK(r.error().code != ErrorCode::Unknown);
            }
        }
        // Zeroed tail and garbage header.
        std::vector<std::uint8_t> z = data;
        std::fill(z.begin() + static_cast<std::ptrdiff_t>(z.size() / 2), z.end(), 0);
        test::write_file(dir / ("z-" + name), z);
        auto r = decode(dir / ("z-" + name), {24, 24});
        CHECK((r ? r->image.valid() : true));
    }
    // Truncated JPEG still yields a (partial) image.
    const auto jpg = test::encode_jpeg(test::quadrants(256, 256));
    test::write_file(
        dir / "half.jpg",
        std::vector<std::uint8_t>(jpg.begin(),
                                  jpg.begin() + static_cast<std::ptrdiff_t>(jpg.size() * 3 / 4)));
    auto half = decode(dir / "half.jpg");
    INFO((half ? std::string() : half.error().message));
    REQUIRE(half);
    CHECK(half->image.size() == PixelSize{256, 256});
    CHECK(test::near(test::pixel(half->image, 4, 4), test::kRed, 40));
    // Truncated non-interlaced PNG: partial image, missing rows transparent.
    const auto png =
        test::encode_png(test::quadrants(64, 64), test::PngOptions{8, 0, false, {}, {}, 0});
    test::write_file(dir / "half.png",
                     std::vector<std::uint8_t>(
                         png.begin(), png.begin() + static_cast<std::ptrdiff_t>(png.size() / 2)));
    auto hp = decode(dir / "half.png");
    REQUIRE(hp);
    CHECK(test::pixel(hp->image, 2, 2) == test::kRed);
    CHECK(test::pixel(hp->image, 2, 63)[3] == 0);
}

TEST_CASE("Cancellation is honoured by decoders", "[decode][cancel]") {
    test::TempDir dir;
    test::write_file(dir / "a.jpg", test::encode_jpeg(test::quadrants(256, 256)));
    test::write_file(dir / "a.png", test::encode_png(test::quadrants(256, 256)));
    test::write_file(dir / "a.webp", test::encode_webp(test::quadrants(256, 256)));
    test::write_file(dir / "a.gif", test::encode_gif(test::quadrants(256, 256)));
    test::write_file(dir / "a.bmp", test::encode_bmp(test::quadrants(256, 256)));
    std::stop_source stop;
    stop.request_stop();
    for (const char* name : {"a.jpg", "a.png", "a.webp", "a.gif", "a.bmp"}) {
        INFO(name);
        pd::DecodeParams p = params({64, 64});
        p.stop = stop.get_token();
        auto r = pd::decode_image_file(dir / name, p);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("Sniffed content wins over the name; unknown formats are Unsupported",
          "[decode][sniff]") {
    test::TempDir dir;
    test::write_file(dir / "really-png.jpg", test::encode_png(test::quadrants(8, 8)));
    const ImagePreview p = decode_ok(dir / "really-png.jpg");
    CHECK(p.info.format == ImageFormat::Png);
    test::write_text(dir / "x.tif", std::string("II*\0junkjunk", 12));
    CHECK(decode(dir / "x.tif").error().code == ErrorCode::Unsupported);
    CHECK(decode(dir / "missing.png").error().code == ErrorCode::NotFound);
    std::filesystem::create_directory(dir / "d.png");
    CHECK(decode(dir / "d.png").error().code == ErrorCode::InvalidArgument);
}

#if defined(NAVIGATOR_HAVE_RSVG)
TEST_CASE("SVG renders through librsvg at the target size", "[decode][svg]") {
    test::TempDir dir;
    test::write_text(dir / "a.svg",
                     "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
                     "<rect width='100' height='100' fill='#ff0000'/>"
                     "<rect x='100' width='100' height='100' fill='#0000ff'/></svg>");
    const ImagePreview s = decode_ok(dir / "a.svg", {100, 100});
    CHECK(s.info.format == ImageFormat::Svg);
    CHECK(s.info.stored == PixelSize{200, 100});
    CHECK(s.image.size() == PixelSize{100, 50});
    CHECK(test::near(test::pixel(s.image, 10, 25), test::kRed, 2));
    CHECK(test::near(test::pixel(s.image, 90, 25), test::kBlue, 2));
    test::write_text(dir / "bad.svg", "<svg><rect");
    CHECK_FALSE(decode(dir / "bad.svg"));
    // External references are not followed (no base file): renders without the image.
    test::write_text(dir / "ext.svg",
                     "<svg xmlns='http://www.w3.org/2000/svg' "
                     "xmlns:xlink='http://www.w3.org/1999/xlink' width='10' height='10'>"
                     "<image xlink:href='/etc/passwd' width='10' height='10'/></svg>");
    auto ext = decode(dir / "ext.svg");
    if (ext) {
        CHECK(test::pixel(ext->image, 5, 5)[3] == 0);
    }
}
#endif

#if defined(NAVIGATOR_HAVE_HEIF)
#include <libheif/heif.h>

namespace {

bool write_heif(const std::filesystem::path& file, const test::Rgba& img,
                heif_compression_format fmt) {
    if (heif_have_encoder_for_format(fmt) == 0) {
        return false;
    }
    heif_context* ctx = heif_context_alloc();
    heif_encoder* enc = nullptr;
    if (heif_context_get_encoder_for_format(ctx, fmt, &enc).code != heif_error_Ok) {
        heif_context_free(ctx);
        return false;
    }
    heif_encoder_set_lossy_quality(enc, 90);
    heif_image* hi = nullptr;
    heif_image_create(static_cast<int>(img.width), static_cast<int>(img.height),
                      heif_colorspace_RGB, heif_chroma_interleaved_RGBA, &hi);
    heif_image_add_plane(hi, heif_channel_interleaved, static_cast<int>(img.width),
                         static_cast<int>(img.height), 8);
    int stride = 0;
    std::uint8_t* plane = heif_image_get_plane(hi, heif_channel_interleaved, &stride);
    for (std::uint32_t y = 0; y < img.height; ++y) {
        std::memcpy(plane + static_cast<std::size_t>(stride) * y,
                    img.px.data() + std::size_t{y} * img.width * 4U, std::size_t{img.width} * 4U);
    }
    const bool ok =
        heif_context_encode_image(ctx, hi, enc, nullptr, nullptr).code == heif_error_Ok &&
        heif_context_write_to_file(ctx, file.c_str()).code == heif_error_Ok;
    heif_image_release(hi);
    heif_encoder_release(enc);
    heif_context_free(ctx);
    return ok;
}

} // namespace

TEST_CASE("HEIC / AVIF decode through libheif when encoders exist", "[decode][heif]") {
    test::TempDir dir;
    for (const auto& [name, fmt, want] :
         {std::tuple{"a.heic", heif_compression_HEVC, ImageFormat::Heif},
          std::tuple{"a.avif", heif_compression_AV1, ImageFormat::Avif}}) {
        INFO(name);
        const auto file = dir / name;
        if (!write_heif(file, test::quadrants(64, 48), fmt)) {
            WARN("no libheif encoder for " << name << "; skipped");
            continue;
        }
        auto r = decode(file, {32, 32});
        if (!r && r.error().code == ErrorCode::Unsupported) {
            WARN("no libheif decoder for " << name << "; skipped");
            continue;
        }
        REQUIRE(r);
        CHECK(r->info.format == want);
        CHECK(r->info.stored == PixelSize{64, 48});
        CHECK(r->image.size() == PixelSize{32, 24});
        CHECK(test::near(test::pixel(r->image, 3, 3), test::kRed, 60));
        CHECK(test::near(test::pixel(r->image, 29, 21), test::kWhite, 60));
        // Truncated file: error, no crash.
        auto bytes = test::read_file(file);
        bytes.resize(bytes.size() / 2);
        test::write_file(dir / "trunc.heif", bytes);
        auto t = decode(dir / "trunc.heif");
        CHECK((t ? t->image.valid() : t.error().code != ErrorCode::Unknown));
    }
}
#endif
