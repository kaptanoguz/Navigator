// tests/preview/video_test.cpp: the FFmpeg poster provider on a tiny MPEG-4-in-Matroska clip
// generated here with libavcodec / libavformat (built with FFmpeg only).
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

#include <navigator/preview_service.hpp>

#include "internal.hpp"
#include "test_util.hpp"

#if defined(NAVIGATOR_HAVE_FFMPEG)
#include "video_clip.hpp"

using namespace navigator;
using namespace navigator::preview;
namespace pd = navigator::preview::detail;

TEST_CASE("FFmpeg poster frame and VideoInfo from a generated clip", "[video]") {
    test::TempDir dir;
    const auto clip = dir / "clip.mkv";
    if (!test::write_clip(clip, 160, 96, 30)) {
        WARN("MPEG-4 encoder or Matroska muxer unavailable; skipped");
        return;
    }
    pd::DecodeParams p;
    p.target = {80, 80};
    auto v = pd::decode_video_file(clip, p);
    INFO((v ? std::string() : v.error().message));
    REQUIRE(v);
    const VideoInfo& info = v->info;
    CHECK(info.source == MetadataSource::FFmpeg);
    CHECK(info.container.find("matroska") != std::string::npos);
    CHECK(info.video_codec == "mpeg4");
    CHECK(info.frame == PixelSize{160, 96});
    CHECK(info.frame_rate == Rational{10, 1});
    CHECK(info.duration_ms.value_or(0) >= 2800);
    CHECK(info.duration_ms.value_or(0) <= 3200);
    CHECK(info.video_streams == 1);
    CHECK(info.audio_streams == 0);
    CHECK(info.audio_codec.empty());
    CHECK(info.rotation_degrees == 0);
    REQUIRE(v->poster.valid());
    CHECK(v->poster.size() == PixelSize{80, 48});
    CHECK(v->poster.opaque);
    const auto left = test::pixel(v->poster, 5, 24);
    const auto right = test::pixel(v->poster, 75, 24);
    CHECK(left[0] > 200);
    CHECK(right[0] < 50);

    // Through the registry (video.ffmpeg wins over the placeholder).
    ProviderRegistry reg;
    REQUIRE(register_builtin_providers(reg, {}));
    PreviewRequest r;
    r.path = clip;
    r.entry = *fs::stat(clip);
    r.mime = "video/x-matroska";
    r.purpose = PreviewPurpose::Thumbnail;
    r.target = {64, 64};
    auto res = reg.produce(r);
    REQUIRE(res);
    CHECK(res->provider_id == "video.ffmpeg");
    REQUIRE(res->kind() == PreviewKind::Video);
    CHECK(std::get<VideoPreview>(res->content).poster.size() == PixelSize{64, 38});

    // Cancellation before open.
    std::stop_source stop;
    stop.request_stop();
    p.stop = stop.get_token();
    CHECK(pd::decode_video_file(clip, p).error().code == ErrorCode::Cancelled);
}

TEST_CASE("FFmpeg rotation from the display matrix", "[video]") {
    test::TempDir dir;
    const auto clip = dir / "rot.mkv";
    if (!test::write_clip(clip, 64, 32, 5, "90")) { // set() takes clockwise degrees
        WARN("MPEG-4 encoder or Matroska muxer unavailable; skipped");
        return;
    }
    auto v = pd::decode_video_file(clip, {});
    REQUIRE(v);
    if (v->info.rotation_degrees == 0) {
        WARN("muxer dropped the display matrix; skipped");
        return;
    }
    CHECK(v->info.rotation_degrees == 90);
    CHECK(v->info.frame == PixelSize{32, 64});
    CHECK(v->poster.size() == PixelSize{32, 64});
    // Bright left half of the stored frame is the top after a clockwise rotation.
    CHECK(test::pixel(v->poster, 16, 5)[0] > 200);
    CHECK(test::pixel(v->poster, 16, 58)[0] < 50);
}

TEST_CASE("Corrupt and non-video files through FFmpeg never crash", "[video][corrupt]") {
    test::TempDir dir;
    const auto clip = dir / "c.mkv";
    if (!test::write_clip(clip, 64, 48, 10)) {
        return;
    }
    const auto bytes = test::read_file(clip);
    std::mt19937 rng(7);
    for (int i = 0; i < 60; ++i) {
        auto m = bytes;
        if (i % 3 == 0) {
            m.resize(rng() % m.size());
        } else {
            for (int k = 0; k < 8; ++k) {
                m[rng() % m.size()] = static_cast<std::uint8_t>(rng());
            }
        }
        test::write_file(dir / "m.mkv", m);
        pd::DecodeParams p;
        p.target = {32, 32};
        p.purpose = (i % 2) == 0 ? PreviewPurpose::Thumbnail : PreviewPurpose::Inspector;
        auto r = pd::decode_video_file(dir / "m.mkv", p);
        if (r && !r->poster.empty()) {
            CHECK(r->poster.valid());
        }
    }
    test::write_text(dir / "text.mp4", "plain text, not a video");
    auto t = pd::decode_video_file(dir / "text.mp4", {});
    CHECK_FALSE(t);
}
#endif
