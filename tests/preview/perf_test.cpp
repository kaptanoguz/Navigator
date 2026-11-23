// tests/preview/perf_test.cpp: decode timings for docs/perf/phase6-preview.md. Hidden ([.perf]):
// run with `navigator_preview_tests "[perf]"` on a Release build.
#include <chrono>
#include <cstdio>
#include <functional>

#include <catch2/catch_test_macros.hpp>

#include <navigator/preview_service.hpp>
#include <navigator/thumbnail.hpp>

#include "internal.hpp"
#include "test_util.hpp"
#include "video_clip.hpp"

using namespace navigator;
using namespace navigator::preview;
namespace pd = navigator::preview::detail;

namespace {

test::Rgba photo_like(std::uint32_t w, std::uint32_t h) {
    test::Rgba img(w, h);
    std::mt19937 rng(42);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = img.at(x, y);
            const auto n = static_cast<int>(rng() % 24U);
            p[0] = static_cast<std::uint8_t>((x * 255U / w + static_cast<unsigned>(n)) & 0xFFU);
            p[1] = static_cast<std::uint8_t>((y * 255U / h + static_cast<unsigned>(n)) & 0xFFU);
            p[2] = static_cast<std::uint8_t>(((x + y) * 128U / (w + h) + static_cast<unsigned>(n)) &
                                             0xFFU);
            p[3] = 255;
        }
    }
    return img;
}

double best_ms(const std::function<void()>& fn, int reps = 5) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        best = std::min(best, ms);
    }
    return best;
}

void time_decode(const char* label, const std::filesystem::path& file, PixelSize target,
                 PreviewPurpose purpose) {
    pd::DecodeParams p;
    p.target = target;
    p.purpose = purpose;
    PixelSize out;
    const double ms = best_ms([&] {
        auto r = pd::decode_image_file(file, p);
        REQUIRE(r);
        out = r->image.size();
    });
    std::printf("%-44s %7.1f ms  -> %ux%u\n", label, ms, out.width, out.height);
}

} // namespace

TEST_CASE("Decode timings", "[.perf]") {
    test::TempDir dir;
    const auto img24 = photo_like(6000, 4000);
    test::JpegOptions jo;
    jo.quality = 90;
    test::write_file(dir / "24mp.jpg", test::encode_jpeg(img24, jo));
    std::printf("24 MP JPEG: %zu bytes\n", std::filesystem::file_size(dir / "24mp.jpg"));
    time_decode("JPEG 6000x4000 thumbnail (128 box, 1/8 DCT)", dir / "24mp.jpg", {128, 128},
                PreviewPurpose::Thumbnail);
    time_decode("JPEG 6000x4000 thumbnail (256 box, 1/8 DCT)", dir / "24mp.jpg", {256, 256},
                PreviewPurpose::Thumbnail);
    time_decode("JPEG 6000x4000 inspector (1920x1080 fit)", dir / "24mp.jpg", {1920, 1080},
                PreviewPurpose::Inspector);
    time_decode("JPEG 6000x4000 inspector 1:1 (full)", dir / "24mp.jpg", {},
                PreviewPurpose::Inspector);

    const auto img12 = photo_like(4000, 3000);
    test::PngOptions po;
    po.color = 1;
    test::write_file(dir / "12mp.png", test::encode_png(img12, po));
    time_decode("PNG 4000x3000 RGB thumbnail (256 box)", dir / "12mp.png", {256, 256},
                PreviewPurpose::Thumbnail);
    time_decode("PNG 4000x3000 RGB inspector (1920x1080 fit)", dir / "12mp.png", {1920, 1080},
                PreviewPurpose::Inspector);
    test::write_file(dir / "12mp.webp", test::encode_webp(img12, false));
    time_decode("WebP 4000x3000 lossy thumbnail (256 box)", dir / "12mp.webp", {256, 256},
                PreviewPurpose::Thumbnail);
    time_decode("WebP 4000x3000 lossy inspector (1920x1080)", dir / "12mp.webp", {1920, 1080},
                PreviewPurpose::Inspector);

    // Thumbnail write + read back through the cache.
    auto thumb = pd::decode_image_file(dir / "24mp.jpg", [] {
        pd::DecodeParams p;
        p.target = {256, 256};
        return p;
    }());
    REQUIRE(thumb);
    thumbnail::SourceInfo si;
    si.path = dir / "24mp.jpg";
    si.uri = *thumbnail::file_uri(si.path);
    const double wms = best_ms([&] {
        REQUIRE(thumbnail::write_thumbnail(dir / "cache", thumbnail::ThumbnailSize::Large, si,
                                           thumb->image, {}));
    });
    const double rms = best_ms([&] {
        auto r =
            thumbnail::lookup_thumbnail(dir / "cache", thumbnail::ThumbnailSize::Large, si, {});
        REQUIRE(r);
        REQUIRE(r->status == thumbnail::LookupStatus::Valid);
    });
    std::printf("%-44s %7.1f ms\n", "Freedesktop write 256x171 PNG", wms);
    std::printf("%-44s %7.1f ms\n", "Freedesktop lookup + decode 256x171", rms);

#if defined(NAVIGATOR_HAVE_FFMPEG)
    const auto clip = dir / "720p.mkv";
    if (test::write_clip(clip, 1280, 720, 100)) {
        for (const PixelSize target : {PixelSize{256, 256}, PixelSize{1920, 1080}}) {
            pd::DecodeParams p;
            p.target = target;
            PixelSize out;
            const double ms = best_ms([&] {
                auto v = pd::decode_video_file(clip, p);
                REQUIRE(v);
                out = v->poster.size();
            });
            std::printf("%-44s %7.1f ms  -> %ux%u\n", "Video poster 1280x720 MPEG-4/MKV (seek 10%)",
                        ms, out.width, out.height);
        }
    }
#endif
}
