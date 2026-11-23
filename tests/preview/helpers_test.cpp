// tests/preview/helpers_test.cpp: pure helpers of navigator/preview.hpp: fit_size, JPEG DCT
// scale choice, EXIF orientation transforms, box downscale, sniffing, keys, approx_bytes and the
// ProviderRegistry.
#include <catch2/catch_test_macros.hpp>

#include <navigator/preview.hpp>

#include "test_util.hpp"

using namespace navigator;
using namespace navigator::preview;

namespace {

ImageBuffer numbered(std::uint32_t w, std::uint32_t h) {
    auto img = make_image({w, h});
    REQUIRE(img);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p =
                img->pixels.data() + std::size_t{y} * img->stride + std::size_t{x} * 4U;
            p[0] = static_cast<std::uint8_t>(x);
            p[1] = static_cast<std::uint8_t>(y);
            p[2] = 0;
            p[3] = 255;
        }
    }
    img->opaque = true;
    return std::move(*img);
}

} // namespace

TEST_CASE("make_image validates sizes", "[helpers]") {
    CHECK_FALSE(make_image({0, 4}));
    CHECK(make_image({0, 4}).error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(make_image({65535, 65535}));
    auto img = make_image({3, 2});
    REQUIRE(img);
    CHECK(img->valid());
    CHECK(img->stride == 12);
    CHECK(img->pixels.size() == 24);
}

TEST_CASE("fit_size keeps aspect and never upscales by default", "[helpers]") {
    CHECK(fit_size({4000, 3000}, {256, 256}) == PixelSize{256, 192});
    CHECK(fit_size({3000, 4000}, {256, 256}) == PixelSize{192, 256});
    CHECK(fit_size({100, 50}, {256, 256}) == PixelSize{100, 50});
    CHECK(fit_size({100, 50}, {256, 256}, true) == PixelSize{256, 128});
    CHECK(fit_size({10000, 1}, {128, 128}) == PixelSize{128, 1});
    CHECK(fit_size({0, 5}, {10, 10}) == PixelSize{0, 0});
    CHECK(fit_size({640, 480}, {0, 0}) == PixelSize{640, 480});
    const PixelSize capped = fit_size({20000, 20000}, {});
    CHECK(capped.pixels() <= kMaxOutputPixels);
    CHECK(capped.width == capped.height);
    CHECK(capped.width > 5000);
}

TEST_CASE("select_jpeg_scale_denom picks the largest denominator not below the target",
          "[helpers]") {
    CHECK(select_jpeg_scale_denom({6000, 4000}, {256, 171}) == 8);
    CHECK(select_jpeg_scale_denom({6000, 4000}, {1600, 1067}) == 2);
    CHECK(select_jpeg_scale_denom({6000, 4000}, {1500, 1000}) == 4);
    CHECK(select_jpeg_scale_denom({6000, 4000}, {6000, 4000}) == 1);
    CHECK(select_jpeg_scale_denom({}, {10, 10}) == 1);
    CHECK(select_jpeg_scale_denom({6000, 4000}, {}) == 1);  // 24 MP fits the output cap
    CHECK(select_jpeg_scale_denom({12000, 9000}, {}) == 2); // 108 MP -> 27 MP
    CHECK(select_jpeg_scale_denom({65535, 65535}, {}) == 8);
}

TEST_CASE("apply_exif_orientation transforms all eight orientations", "[helpers]") {
    // 3 x 2 source with pixel (x, y) tagged.
    const ImageBuffer src = numbered(3, 2);
    struct Case {
        ExifOrientation o;
        PixelSize size;
        // display (0,0) and display (w-1, 0) come from these stored pixels
        std::pair<int, int> tl;
        std::pair<int, int> tr;
    };
    const Case cases[] = {
        {ExifOrientation::Normal, {3, 2}, {0, 0}, {2, 0}},
        {ExifOrientation::FlipHorizontal, {3, 2}, {2, 0}, {0, 0}},
        {ExifOrientation::Rotate180, {3, 2}, {2, 1}, {0, 1}},
        {ExifOrientation::FlipVertical, {3, 2}, {0, 1}, {2, 1}},
        {ExifOrientation::Transpose, {2, 3}, {0, 0}, {0, 1}},
        {ExifOrientation::Rotate90, {2, 3}, {0, 1}, {0, 0}},
        {ExifOrientation::Transverse, {2, 3}, {2, 1}, {2, 0}},
        {ExifOrientation::Rotate270, {2, 3}, {2, 0}, {2, 1}},
    };
    for (const Case& c : cases) {
        INFO("orientation " << static_cast<int>(c.o));
        auto out = apply_exif_orientation(src, c.o);
        REQUIRE(out);
        CHECK(out->size() == c.size);
        CHECK(out->valid());
        CHECK(out->opaque);
        const auto tl = test::pixel(*out, 0, 0);
        const auto tr = test::pixel(*out, out->width - 1, 0);
        CHECK(tl[0] == c.tl.first);
        CHECK(tl[1] == c.tl.second);
        CHECK(tr[0] == c.tr.first);
        CHECK(tr[1] == c.tr.second);
    }
    CHECK_FALSE(apply_exif_orientation(ImageBuffer{}, ExifOrientation::Rotate90));
}

TEST_CASE("downscale averages areas of premultiplied pixels", "[helpers]") {
    auto img = make_image({4, 2});
    REQUIRE(img);
    // Left half opaque white, right half transparent.
    for (std::uint32_t y = 0; y < 2; ++y) {
        for (std::uint32_t x = 0; x < 2; ++x) {
            std::uint8_t* p =
                img->pixels.data() + std::size_t{y} * img->stride + std::size_t{x} * 4U;
            p[0] = p[1] = p[2] = p[3] = 255;
        }
    }
    auto d = downscale(*img, {2, 1});
    REQUIRE(d);
    CHECK(test::pixel(*d, 0, 0) == std::array<std::uint8_t, 4>{255, 255, 255, 255});
    CHECK(test::pixel(*d, 1, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    auto one = downscale(*img, {1, 1});
    REQUIRE(one);
    const auto p = test::pixel(*one, 0, 0);
    CHECK(p[3] >= 127);
    CHECK(p[3] <= 128);
    CHECK(p[0] == p[3]); // stays premultiplied (colour <= alpha)
    CHECK_FALSE(one->opaque);

    // Non-integer ratio: 5 -> 3, total mass preserved.
    const ImageBuffer ramp = numbered(5, 1);
    auto r = downscale(ramp, {3, 1});
    REQUIRE(r);
    CHECK(r->width == 3);
    CHECK(test::pixel(*r, 0, 0)[0] <= 1);
    CHECK(test::pixel(*r, 2, 0)[0] >= 3);
    CHECK(r->opaque);

    CHECK_FALSE(downscale(*img, {5, 1}));
    CHECK_FALSE(downscale(*img, {0, 1}));
    auto same = downscale(*img, {4, 2});
    REQUIRE(same);
    CHECK(same->pixels == img->pixels);
}

TEST_CASE("sniff_image_format recognises magic numbers", "[helpers]") {
    const auto sniff = [](std::string_view s) {
        return sniff_image_format(
            Span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    };
    CHECK(sniff("\xFF\xD8\xFF\xE0") == ImageFormat::Jpeg);
    CHECK(sniff("\x89PNG\r\n\x1a\n") == ImageFormat::Png);
    CHECK(sniff(std::string_view("RIFF\0\0\0\0WEBPVP8 ", 16)) == ImageFormat::WebP);
    CHECK(sniff("GIF89a") == ImageFormat::Gif);
    CHECK(sniff("GIF87a") == ImageFormat::Gif);
    CHECK(sniff(std::string_view("BM\0\0\0\0\0\0\0\0\0\0\0\0\x28\0\0\0", 18)) == ImageFormat::Bmp);
    CHECK(sniff(std::string_view("BM\0\0\0\0\0\0\0\0\0\0\0\0\x99\0\0\0", 18)) ==
          ImageFormat::Unknown);
    CHECK(sniff(std::string_view("\0\0\0\x18"
                                 "ftypheic\0\0\0\0mif1heic",
                                 24)) == ImageFormat::Heif);
    CHECK(sniff(std::string_view("\0\0\0\x1c"
                                 "ftypavif\0\0\0\0avifmif1miaf",
                                 28)) == ImageFormat::Avif);
    CHECK(sniff(std::string_view("\0\0\0\x18"
                                 "ftypmif1\0\0\0\0mif1avif",
                                 24)) == ImageFormat::Avif);
    CHECK(sniff(std::string_view("\0\0\0\x18"
                                 "ftypisom\0\0\0\0isommp41",
                                 24)) == ImageFormat::Unknown);
    CHECK(sniff(std::string_view("II*\0", 4)) == ImageFormat::Tiff);
    CHECK(sniff(std::string_view("MM\0*", 4)) == ImageFormat::Tiff);
    CHECK(sniff(std::string_view("\0\0\1\0", 4)) == ImageFormat::Ico);
    CHECK(sniff("  \n<svg xmlns=") == ImageFormat::Svg);
    CHECK(sniff("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<svg>") == ImageFormat::Svg);
    CHECK(sniff("<?xml version=\"1.0\"?><html>") == ImageFormat::Unknown);
    CHECK(sniff("") == ImageFormat::Unknown);
    CHECK(to_string(ImageFormat::Avif) == "avif");
    CHECK(to_string(ImageFormat::Unknown) == "unknown");
}

TEST_CASE("make_key and hash_value follow the cache identity", "[helpers][keys]") {
    PreviewRequest r;
    r.path = "/tmp/a.png";
    r.entry.kind = fs::EntryKind::File;
    r.entry.size = 10;
    r.entry.mtime = Timestamp{std::chrono::seconds(100)};
    r.target = {128, 128};
    r.purpose = PreviewPurpose::Thumbnail;
    const PreviewKey k1 = make_key(r);
    CHECK(k1.options == 0);
    CHECK(k1.path == r.path);
    CHECK(k1 == make_key(r));
    CHECK(hash_value(k1) == hash_value(make_key(r)));

    PreviewRequest changed = r;
    changed.entry.mtime += std::chrono::seconds(1);
    CHECK_FALSE(make_key(changed) == k1);
    changed = r;
    changed.entry.size = 11;
    CHECK_FALSE(make_key(changed) == k1);
    changed = r;
    changed.target = {256, 256};
    CHECK_FALSE(make_key(changed) == k1);

    PreviewRequest insp = r;
    insp.purpose = PreviewPurpose::Inspector;
    const PreviewKey ki = make_key(insp);
    CHECK(ki.options != 0);
    insp.text.max_bytes = 1024;
    CHECK(make_key(insp).options != ki.options);

    PreviewRequest folder = insp;
    folder.entry.kind = fs::EntryKind::Directory;
    const PreviewKey kf = make_key(folder);
    folder.text.max_bytes = 2048; // irrelevant for folders
    CHECK(make_key(folder).options == kf.options);
    folder.folder.show_hidden = true;
    CHECK(make_key(folder).options != kf.options);
}

TEST_CASE("approx_bytes counts pixel buffers and text", "[helpers][keys]") {
    PreviewResult r;
    const std::size_t base = approx_bytes(r);
    ImagePreview ip;
    ip.image = *make_image({100, 100});
    r.content = std::move(ip);
    CHECK(approx_bytes(r) >= 40000);
    CHECK(base >= sizeof(PreviewResult));
    TextPreview tp;
    tp.text.assign(5000, 'x');
    tp.rows.resize(10);
    r.content = std::move(tp);
    CHECK(approx_bytes(r) >= 5000 + 10 * sizeof(TextRow));
}

namespace {

class FakeProvider final : public IPreviewProvider {
public:
    FakeProvider(std::string id, int prio, ErrorCode err = ErrorCode::Unknown)
        : id_(std::move(id)), prio_(prio), err_(err) {}
    std::string_view id() const noexcept override { return id_; }
    int can_handle(std::string_view mime, const fs::DirEntry&,
                   PreviewPurpose) const noexcept override {
        return mime == "image/x-test" || mime == "application/octet-stream" ? prio_ : kCannotHandle;
    }
    Result<PreviewResult> produce(const PreviewRequest&) const override {
        if (err_ != ErrorCode::Unknown) {
            return fail(err_, "fake");
        }
        PreviewResult r;
        r.content = PlaceholderPreview{PlaceholderReason::ComingSoon, id_};
        return r;
    }

private:
    std::string id_;
    int prio_;
    ErrorCode err_;
};

} // namespace

TEST_CASE("ProviderRegistry orders candidates and falls through Unsupported", "[registry]") {
    ProviderRegistry reg;
    CHECK(reg.add(nullptr).error().code == ErrorCode::InvalidArgument);
    REQUIRE(reg.add(std::make_unique<FakeProvider>("low", kPriorityGeneric)));
    REQUIRE(
        reg.add(std::make_unique<FakeProvider>("high", kPriorityNative, ErrorCode::Unsupported)));
    REQUIRE(reg.add(std::make_unique<FakeProvider>("tie", kPriorityGeneric)));
    CHECK(reg.add(std::make_unique<FakeProvider>("low", 1)).error().code ==
          ErrorCode::AlreadyExists);
    CHECK(reg.size() == 3);
    CHECK(reg.find("tie") != nullptr);
    CHECK(reg.find("none") == nullptr);

    const auto c = reg.candidates("image/x-test", {}, PreviewPurpose::Inspector);
    REQUIRE(c.size() == 3);
    CHECK(c[0]->id() == "high");
    CHECK(c[1]->id() == "low");
    CHECK(c[2]->id() == "tie");
    CHECK(reg.candidates("", {}, PreviewPurpose::Inspector).size() == 3); // octet-stream

    PreviewRequest req;
    req.mime = "image/x-test";
    auto r = reg.produce(req);
    REQUIRE(r);
    CHECK(r->provider_id == "low");

    req.mime = "text/none";
    r = reg.produce(req);
    REQUIRE(r);
    CHECK(r->provider_id.empty());
    REQUIRE(r->kind() == PreviewKind::Unsupported);
    CHECK(std::get<PlaceholderPreview>(r->content).reason == PlaceholderReason::NoProvider);

    ProviderRegistry failing;
    REQUIRE(failing.add(std::make_unique<FakeProvider>("bad", 5, ErrorCode::InvalidArgument)));
    REQUIRE(failing.add(std::make_unique<FakeProvider>("good", 1)));
    req.mime = "image/x-test";
    CHECK(failing.produce(req).error().code == ErrorCode::InvalidArgument);

    std::stop_source stop;
    stop.request_stop();
    req.stop = stop.get_token();
    CHECK(reg.produce(req).error().code == ErrorCode::Cancelled);
}
