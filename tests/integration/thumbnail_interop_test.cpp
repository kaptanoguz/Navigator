// Freedesktop thumbnail interop (owner: qa), Phase 6. Temporary cache roots only (never
// ~/.cache/thumbnails). Checks with an independent PNG chunk reader, the system md5sum and
// ffmpeg's PNG decoder that:
//   - thumbnails written by Navigator's service are spec-valid (name = md5 of the GLib-style URI,
//     class directory, size, tEXt keys before IDAT, 0600 file, 0700 directories, no temporary
//     files left) and are read back by Navigator (disk hit, no second decode);
//   - a thumbnail made by a system tool (gdk-pixbuf-thumbnailer) with Thumb::URI / Thumb::MTime
//     added the way GNOME's thumbnail factory saves them is accepted as Valid and reused; it goes
//     Stale when the file is touched and is regenerated (overwritten with Navigator's keys);
//   - failure markers: written for a corrupt file under fail/navigator-1, honoured on the next
//     run (KnownFailure, no decode), retried after a touch; other programs' fail dirs are ignored.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/thumbnail.hpp>

#include "fixture.hpp"
#include "preview_support.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using preview::PixelSize;
using preview::PreviewPurpose;
using thumbnail::LookupStatus;
using thumbnail::ThumbnailSize;

namespace {

// The URI GLib's g_filename_to_uri() writes for a path made of safe bytes plus one name with a
// space and a Turkish letter (encoded by hand here, not by thumbnail::file_uri).
std::string glib_uri(const Path& dir, std::string_view encoded_name) {
    return "file://" + dir.string() + "/" + std::string(encoded_name);
}

void require_tools(std::initializer_list<std::string_view> tools) {
    for (const std::string_view t : tools) {
        if (!have_program(t)) {
            SKIP(std::string(t) + " is not installed");
        }
    }
}

} // namespace

TEST_CASE("Thumbnails written by Navigator are spec-valid and read back",
          "[preview][thumbnail][interop]") {
    require_tools({"ffmpeg", "md5sum", "sh"});
    TempDir t;
    const Path root = t / "cache" / "thumbnails";
    std::filesystem::create_directories(t / "data");
    const Path src = t / "data" / "fotoğraf 1.png";
    REQUIRE(ffmpeg_image(src, 640, 480));
    const Path small = t / "data" / "small.png";
    REQUIRE(ffmpeg_image(small, 100, 50));

    const std::string uri = glib_uri(t / "data", "foto%C4%9Fraf%201.png");
    REQUIRE(thumbnail::file_uri(src).value_or("") == uri);
    const std::string md5 = system_md5(t.path(), uri);
    REQUIRE(md5.size() == 32);
    CHECK(thumbnail::md5_hex(uri) == md5);
    const Path large = root / "large" / (md5 + ".png");
    CHECK(thumbnail::thumbnail_path(root, ThumbnailSize::Large, uri) == large);

    {
        std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
        REQUIRE(s);
        // A 48 dp icon at scale 4 asks for 192 px: the Large class (256) is generated.
        const Delivery c = produce_one(*s, request_for(src, PreviewPurpose::Thumbnail, {192, 192}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        REQUIRE(c->result.has_value());
        const preview::ImagePreview* img = image_of(*c);
        REQUIRE(img != nullptr);
        CHECK(img->origin == preview::ImageOrigin::Decoded);
        CHECK(img->image.size().width == 192); // downscaled to the request after the write
        CHECK(img->image.size().height == 144);
        // The small source: never upscaled.
        const Delivery c2 =
            produce_one(*s, request_for(small, PreviewPurpose::Thumbnail, {256, 256}),
                        preview::RequestPriority::Visible);
        REQUIRE(c2.has_value());
        REQUIRE(image_of(*c2) != nullptr);
        CHECK(s->stats().thumbnails_written == 2);
        CHECK(s->stats().produced == 2);
        s->shutdown();
    }

    // On disk, read with an independent PNG parser.
    REQUIRE(std::filesystem::is_regular_file(large));
    const PngChunks png = parse_png(read_bytes(large));
    CHECK(png.signature);
    CHECK(png.crc_ok);
    CHECK(png.complete);
    CHECK(png.width == 256); // the longer side equals the class size
    CHECK(png.height == 192);
    CHECK(png.text_before_idat == png.text.size()); // keys are readable without inflating IDAT
    const fs::DirEntry e = entry_of(src).value_or(fs::DirEntry{});
    CHECK(png.key("Thumb::URI") == uri);
    CHECK(png.key("Thumb::MTime") == std::to_string(thumbnail::mtime_seconds(e.mtime)));
    CHECK(png.key("Thumb::Size") == std::to_string(e.size));
    CHECK(png.key("Thumb::Mimetype") == "image/png");
    CHECK(png.key("Thumb::Image::Width") == "640");
    CHECK(png.key("Thumb::Image::Height") == "480");
    CHECK(png.key("Software").starts_with("Navigator"));
    // Permissions: file 0600, the directories Navigator created 0700.
    CHECK(mode_of(large) == 0600);
    CHECK(mode_of(root / "large") == 0700);
    CHECK(mode_of(root) == 0700);
    // The small source stays 100 x 50 in the Large class.
    const std::string small_uri = thumbnail::file_uri(small).value_or("");
    const PngChunks sp =
        parse_png(read_bytes(root / "large" / (system_md5(t.path(), small_uri) + ".png")));
    CHECK(sp.width == 100);
    CHECK(sp.height == 50);
    // Exactly the two thumbnails, no temporary files, no failure markers.
    std::vector<std::string> want{"large/" + md5 + ".png",
                                  "large/" + system_md5(t.path(), small_uri) + ".png"};
    std::sort(want.begin(), want.end());
    CHECK(files_below(root) == want);
    // Another decoder accepts it (what Nautilus / Nemo would load).
    CHECK(run_tool(
              {"ffmpeg", "-nostdin", "-v", "error", "-i", large.string(), "-f", "null", "-"}) == 0);

    // Navigator reads it back: Valid, and a fresh service uses it without decoding.
    const Result<thumbnail::ThumbnailRead> back =
        thumbnail::lookup_thumbnail(root, ThumbnailSize::Large, source_of(src), {});
    REQUIRE(back.has_value());
    CHECK(back->status == LookupStatus::Valid);
    CHECK(back->file == large);
    CHECK(back->original == PixelSize{640, 480});
    std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
    REQUIRE(s);
    const Delivery c = produce_one(*s, request_for(src, PreviewPurpose::Thumbnail, {256, 256}),
                                   preview::RequestPriority::Visible);
    REQUIRE(c.has_value());
    REQUIRE(image_of(*c) != nullptr);
    CHECK(image_of(*c)->origin == preview::ImageOrigin::ThumbnailCache);
    CHECK(s->stats().disk_hits == 1);
    CHECK(s->stats().produced == 0);
    CHECK(s->stats().thumbnails_written == 0);
}

TEST_CASE("A GNOME-style thumbnail from gdk-pixbuf-thumbnailer is Valid, then Stale after a touch",
          "[preview][thumbnail][interop]") {
    require_tools({"ffmpeg", "gdk-pixbuf-thumbnailer", "md5sum", "sh"});
    TempDir t;
    const Path root = t / "thumbnails";
    std::filesystem::create_directories(t / "data");
    const Path src = t / "data" / "picture.png";
    REQUIRE(ffmpeg_image(src, 400, 300));
    const std::string uri = glib_uri(t / "data", "picture.png");
    const std::string md5 = system_md5(t.path(), uri);
    REQUIRE(md5.size() == 32);

    // gdk-pixbuf-thumbnailer -s 128 <in> <out>, then the keys GNOME's thumbnail factory saves
    // (gnome_desktop_thumbnail_factory_save_thumbnail: Thumb::URI, Thumb::MTime, Software).
    const Path raw = t / "raw.png";
    REQUIRE(run_tool({"gdk-pixbuf-thumbnailer", "-s", "128", src.string(), raw.string()}) == 0);
    const PngChunks raw_png = parse_png(read_bytes(raw));
    REQUIRE(raw_png.signature);
    CHECK(raw_png.width == 128);
    const fs::DirEntry e = entry_of(src).value_or(fs::DirEntry{});
    const std::string mtime = std::to_string(thumbnail::mtime_seconds(e.mtime));
    const Path normal = root / "normal" / (md5 + ".png");
    std::filesystem::create_directories(root / "normal");
    write_bytes(normal,
                add_text_chunks(read_bytes(raw), {{"Thumb::URI", uri},
                                                  {"Thumb::MTime", mtime},
                                                  {"Software", "GNOME::ThumbnailFactory"}}));
    REQUIRE(::chmod(normal.c_str(), 0600) == 0);
    REQUIRE(parse_png(read_bytes(normal)).crc_ok);

    SECTION("Valid and reused") {
        const Result<thumbnail::ThumbnailRead> r =
            thumbnail::lookup_thumbnail(root, ThumbnailSize::Normal, source_of(src), {});
        REQUIRE(r.has_value());
        CHECK(r->status == LookupStatus::Valid);
        CHECK(r->file == normal);
        CHECK(r->image.size() == PixelSize{128, 96});
        CHECK(r->original == PixelSize{}); // GNOME wrote no Thumb::Image::* keys
        std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
        REQUIRE(s);
        const Delivery c = produce_one(*s, request_for(src, PreviewPurpose::Thumbnail, {96, 96}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        REQUIRE(image_of(*c) != nullptr);
        CHECK(image_of(*c)->origin == preview::ImageOrigin::ThumbnailCache);
        CHECK(image_of(*c)->image.size() == PixelSize{96, 72});
        CHECK(s->stats().disk_hits == 1);
        CHECK(s->stats().produced == 0);
        CHECK(s->stats().thumbnails_written == 0);
        // Untouched: GNOME's file stays byte for byte.
        CHECK(parse_png(read_bytes(normal)).key("Software") == "GNOME::ThumbnailFactory");
    }
    SECTION("Wrong URI is Invalid") {
        write_bytes(normal, add_text_chunks(read_bytes(raw),
                                            {{"Thumb::URI", uri + "x"}, {"Thumb::MTime", mtime}}));
        const Result<thumbnail::ThumbnailRead> r =
            thumbnail::lookup_thumbnail(root, ThumbnailSize::Normal, source_of(src), {});
        REQUIRE(r.has_value());
        CHECK(r->status == LookupStatus::Invalid);
    }
    SECTION("Missing MTime is Invalid") {
        write_bytes(normal, add_text_chunks(read_bytes(raw), {{"Thumb::URI", uri}}));
        const Result<thumbnail::ThumbnailRead> r =
            thumbnail::lookup_thumbnail(root, ThumbnailSize::Normal, source_of(src), {});
        REQUIRE(r.has_value());
        CHECK(r->status != LookupStatus::Valid);
    }
    SECTION("Stale after touching the file, then regenerated") {
        REQUIRE(shift_mtime(src, 10));
        const Result<thumbnail::ThumbnailRead> r =
            thumbnail::lookup_thumbnail(root, ThumbnailSize::Normal, source_of(src), {});
        REQUIRE(r.has_value());
        CHECK(r->status == LookupStatus::Stale);
        std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
        REQUIRE(s);
        const Delivery c = produce_one(*s, request_for(src, PreviewPurpose::Thumbnail, {128, 128}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        REQUIRE(image_of(*c) != nullptr);
        CHECK(image_of(*c)->origin == preview::ImageOrigin::Decoded);
        CHECK(s->stats().disk_hits == 0);
        CHECK(s->stats().thumbnails_written == 1);
        const PngChunks now = parse_png(read_bytes(normal));
        const fs::DirEntry e2 = entry_of(src).value_or(fs::DirEntry{});
        CHECK(now.key("Thumb::MTime") == std::to_string(thumbnail::mtime_seconds(e2.mtime)));
        CHECK(now.key("Thumb::URI") == uri);
        CHECK(now.key("Software").starts_with("Navigator"));
        CHECK(mode_of(normal) == 0600);
        CHECK(thumbnail::lookup_thumbnail(root, ThumbnailSize::Normal, source_of(src), {})
                  .value_or(thumbnail::ThumbnailRead{})
                  .status == LookupStatus::Valid);
    }
}

TEST_CASE("Failure markers: written for a corrupt file, honoured, retried after a touch",
          "[preview][thumbnail][interop]") {
    require_tools({"ffmpeg", "md5sum", "sh"});
    TempDir t;
    const Path root = t / "thumbnails";
    std::filesystem::create_directories(t / "data");
    const Path good = t / "data" / "good.png";
    REQUIRE(ffmpeg_image(good, 64, 64));
    // PNG signature and IHDR of the good file, then garbage: a corrupt PNG.
    std::vector<std::uint8_t> bytes = read_bytes(good);
    REQUIRE(bytes.size() > 200);
    for (std::size_t i = 40; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 37U);
    }
    const Path bad = t / "data" / "bad.png";
    write_bytes(bad, bytes);
    const std::string bad_uri = glib_uri(t / "data", "bad.png");
    const Path marker = root / "fail" / "navigator-1" / (system_md5(t.path(), bad_uri) + ".png");
    CHECK(thumbnail::failure_path(root, bad_uri) == marker);

    {
        std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
        REQUIRE(s);
        const Delivery c = produce_one(*s, request_for(bad, PreviewPurpose::Thumbnail, {128, 128}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        CHECK(image_of(*c) == nullptr);
        CHECK(s->stats().failures_written == 1);
        CHECK(s->stats().thumbnails_written == 0);
    }
    REQUIRE(std::filesystem::is_regular_file(marker));
    const PngChunks m = parse_png(read_bytes(marker));
    CHECK(m.crc_ok);
    CHECK(m.width == 1);
    CHECK(m.height == 1);
    CHECK(m.key("Thumb::URI") == bad_uri);
    CHECK(m.key("Thumb::MTime") ==
          std::to_string(thumbnail::mtime_seconds(entry_of(bad).value_or(fs::DirEntry{}).mtime)));
    CHECK(mode_of(marker) == 0600);
    CHECK(mode_of(root / "fail" / "navigator-1") == 0700);
    CHECK(thumbnail::has_failure(root, source_of(bad)).value_or(false));
    // Nothing but the marker.
    CHECK(files_below(root).size() == 1);

    {
        // Next run: KnownFailure, the decoder is not tried again.
        std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
        REQUIRE(s);
        const Delivery c = produce_one(*s, request_for(bad, PreviewPurpose::Thumbnail, {128, 128}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        const preview::PlaceholderPreview* p = placeholder_of(*c);
        REQUIRE(p != nullptr);
        CHECK(p->reason == preview::PlaceholderReason::KnownFailure);
        CHECK(s->stats().produced == 0);
        CHECK(s->stats().failures_written == 0);
        // The Inspector still decodes the one inspected file (and reports it broken).
        const Delivery ins =
            produce_one(*s, request_for(bad, PreviewPurpose::Inspector, {512, 512}));
        REQUIRE(ins.has_value());
        CHECK(image_of(*ins) == nullptr);
    }

    {
        // The file changed: the marker no longer matches; retried (and fails again).
        REQUIRE(shift_mtime(bad, 10));
        CHECK_FALSE(thumbnail::has_failure(root, source_of(bad)).value_or(true));
        std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
        REQUIRE(s);
        const Delivery c = produce_one(*s, request_for(bad, PreviewPurpose::Thumbnail, {128, 128}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        const bool known = placeholder_of(*c) != nullptr &&
                           placeholder_of(*c)->reason == preview::PlaceholderReason::KnownFailure;
        CHECK_FALSE(known);
        CHECK(s->stats().failures_written == 1);
        CHECK(thumbnail::has_failure(root, source_of(bad)).value_or(false));
    }

    {
        // Another program's failure marker for a GOOD file is never read: it is thumbnailed.
        const std::string good_uri = glib_uri(t / "data", "good.png");
        const Path gnome_fail =
            root / "fail" / "gnome-thumbnail-factory" / (system_md5(t.path(), good_uri) + ".png");
        std::filesystem::create_directories(gnome_fail.parent_path());
        const fs::DirEntry ge = entry_of(good).value_or(fs::DirEntry{});
        write_bytes(gnome_fail,
                    add_text_chunks(
                        read_bytes(good),
                        {{"Thumb::URI", good_uri},
                         {"Thumb::MTime", std::to_string(thumbnail::mtime_seconds(ge.mtime))}}));
        std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
        REQUIRE(s);
        const Delivery c = produce_one(*s, request_for(good, PreviewPurpose::Thumbnail, {128, 128}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        REQUIRE(image_of(*c) != nullptr);
        CHECK(s->stats().thumbnails_written == 1);
    }
}

TEST_CASE("A read-only cache root is used but never written", "[preview][thumbnail][interop]") {
    require_tools({"ffmpeg"});
    if (::geteuid() == 0) {
        SKIP("root ignores permissions");
    }
    TempDir t;
    const Path root = t / "thumbnails";
    std::filesystem::create_directories(root / "normal");
    std::filesystem::create_directories(t / "data");
    const Path src = t / "data" / "a.png";
    REQUIRE(ffmpeg_image(src, 64, 48));
    REQUIRE(::chmod(root.c_str(), 0500) == 0);
    REQUIRE(::chmod((root / "normal").c_str(), 0500) == 0);
    {
        std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root));
        REQUIRE(s);
        const Delivery c = produce_one(*s, request_for(src, PreviewPurpose::Thumbnail, {128, 128}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        // The thumbnail is still shown (a write failure never hides the decoded image).
        CHECK(image_of(*c) != nullptr);
        CHECK(s->stats().thumbnails_written == 0);
    }
    (void)::chmod((root / "normal").c_str(), 0700);
    (void)::chmod(root.c_str(), 0700);
    CHECK(files_below(root).empty());
}
