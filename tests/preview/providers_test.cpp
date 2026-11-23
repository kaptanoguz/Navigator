// tests/preview/providers_test.cpp: register_builtin_providers ids and priorities, the text,
// folder and placeholder providers, sniffing extensionless images, external thumbnailer
// providers through a fake runner.
#include <catch2/catch_test_macros.hpp>

#include <navigator/preview_service.hpp>

#include "fake_runner.hpp"
#include "test_util.hpp"

using namespace navigator;
using namespace navigator::preview;

namespace {

fs::DirEntry file_entry(const std::filesystem::path& p) {
    auto e = fs::stat(p, false);
    REQUIRE(e);
    return *e;
}

PreviewRequest request(const std::filesystem::path& p, std::string mime,
                       PreviewPurpose purpose = PreviewPurpose::Inspector) {
    PreviewRequest r;
    r.path = p;
    r.entry = file_entry(p);
    r.mime = std::move(mime);
    r.purpose = purpose;
    return r;
}

std::shared_ptr<ProviderRegistry> builtin(const BuiltinProviderOptions& opt = {}) {
    auto reg = std::make_shared<ProviderRegistry>();
    REQUIRE(register_builtin_providers(*reg, opt));
    return reg;
}

} // namespace

TEST_CASE("build_features reports the required decoders", "[providers]") {
    const BuildFeatures f = build_features();
    CHECK(f.jpeg);
    CHECK(f.png);
    CHECK(f.webp);
    CHECK(f.gif);
    CHECK(f.bmp);
    CHECK(f.exif);
#if defined(NAVIGATOR_HAVE_FFMPEG)
    CHECK(f.ffmpeg);
#else
    CHECK_FALSE(f.ffmpeg);
#endif
#if defined(NAVIGATOR_HAVE_RSVG)
    CHECK(f.svg);
#else
    CHECK_FALSE(f.svg);
#endif
}

TEST_CASE("register_builtin_providers registers the documented ids", "[providers]") {
    auto reg = builtin();
    for (const char* id :
         {"image.jpeg", "image.png", "image.webp", "image.gif", "image.bmp", "text.utf8",
          "folder.listing", "placeholder.coming_soon", "placeholder.video"}) {
        INFO(id);
        CHECK(reg->find(id) != nullptr);
    }
    CHECK((reg->find("image.svg") != nullptr) == build_features().svg);
    CHECK((reg->find("video.ffmpeg") != nullptr) == build_features().ffmpeg);
    CHECK((reg->find("image.heif") != nullptr) == (build_features().heif || build_features().avif));
    CHECK(register_builtin_providers(*reg, {}).error().code == ErrorCode::AlreadyExists);

    fs::DirEntry file;
    file.kind = fs::EntryKind::File;
    auto c = reg->candidates("image/jpeg", file, PreviewPurpose::Thumbnail);
    REQUIRE_FALSE(c.empty());
    CHECK(c[0]->id() == "image.jpeg");
    c = reg->candidates("application/pdf", file, PreviewPurpose::Inspector);
    REQUIRE(c.size() == 1);
    CHECK(c[0]->id() == "placeholder.coming_soon");
    c = reg->candidates("video/mp4", file, PreviewPurpose::Inspector);
    REQUIRE_FALSE(c.empty());
    CHECK(c.back()->id() == "placeholder.video");
    c = reg->candidates("text/plain", file, PreviewPurpose::Thumbnail);
    CHECK(c.empty()); // no text thumbnails
    fs::DirEntry dir;
    dir.kind = fs::EntryKind::Directory;
    c = reg->candidates("inode/directory", dir, PreviewPurpose::Inspector);
    REQUIRE(c.size() == 1);
    CHECK(c[0]->id() == "folder.listing");
    CHECK(reg->candidates("inode/directory", dir, PreviewPurpose::Thumbnail).empty());
    fs::DirEntry big = file;
    big.size = 2U << 20U;
    CHECK(reg->candidates("application/octet-stream", big, PreviewPurpose::Inspector).empty());
    big.size = 100;
    CHECK(reg->candidates("application/octet-stream", big, PreviewPurpose::Inspector).size() == 1);
}

TEST_CASE("Text provider: text, binary placeholder, extensionless image", "[providers][text]") {
    test::TempDir dir;
    auto reg = builtin();
    test::write_text(dir / "a.txt", "hello\nworld\n");
    auto r = reg->produce(request(dir / "a.txt", "text/plain"));
    REQUIRE(r);
    CHECK(r->provider_id == "text.utf8");
    REQUIRE(r->kind() == PreviewKind::Text);
    CHECK(std::get<TextPreview>(r->content).line_count == 2);

    test::write_text(dir / "blob", std::string("\x7F"
                                               "ELF\0\0\0\0",
                                               8));
    r = reg->produce(request(dir / "blob", "application/octet-stream"));
    REQUIRE(r);
    REQUIRE(r->kind() == PreviewKind::Unsupported);
    CHECK(std::get<PlaceholderPreview>(r->content).reason == PlaceholderReason::Binary);

    test::write_file(dir / "photo", test::encode_jpeg(test::quadrants(32, 16)));
    r = reg->produce(request(dir / "photo", "application/octet-stream"));
    REQUIRE(r);
    REQUIRE(r->kind() == PreviewKind::Image);
    CHECK(std::get<ImagePreview>(r->content).info.format == ImageFormat::Jpeg);

    // Text caps follow the request options.
    test::write_text(dir / "big.log", std::string(10000, 'x'));
    PreviewRequest capped = request(dir / "big.log", "text/x-log");
    capped.text.max_bytes = 1000;
    r = reg->produce(capped);
    REQUIRE(r);
    const auto& tp = std::get<TextPreview>(r->content);
    CHECK(tp.truncated);
    CHECK(tp.file_size == 10000);
    CHECK(tp.text.size() == 1000);
}

TEST_CASE("Folder provider lists without navigating, sorted and capped", "[providers][folder]") {
    test::TempDir dir;
    auto reg = builtin();
    const auto f = dir / "f";
    std::filesystem::create_directories(f / "sub2");
    std::filesystem::create_directories(f / "Sub1");
    test::write_text(f / "b.txt", "x");
    test::write_text(f / "a10.txt", "x");
    test::write_text(f / "a9.txt", "x");
    test::write_text(f / ".hidden", "x");
    PreviewRequest req = request(f, "inode/directory");
    auto r = reg->produce(req);
    REQUIRE(r);
    CHECK(r->provider_id == "folder.listing");
    const auto& fp = std::get<FolderPreview>(r->content);
    CHECK(fp.total == 5);
    CHECK(fp.folders == 2);
    CHECK(fp.files == 3);
    CHECK(fp.hidden == 1);
    CHECK_FALSE(fp.truncated);
    REQUIRE(fp.entries.size() == 5);
    CHECK(fp.entries[0].name == "Sub1");
    CHECK(fp.entries[1].name == "sub2");
    CHECK(fp.entries[2].name == "a9.txt");
    CHECK(fp.entries[3].name == "a10.txt");

    req.folder.show_hidden = true;
    req.folder.max_entries = 2;
    r = reg->produce(req);
    REQUIRE(r);
    const auto& capped = std::get<FolderPreview>(r->content);
    CHECK(capped.total == 6);
    CHECK(capped.hidden == 0);
    CHECK(capped.entries.size() == 2);
    CHECK(capped.truncated);

    std::filesystem::remove_all(f);
    CHECK(reg->produce(req).error().code == ErrorCode::NotFound);
}

TEST_CASE("Placeholders: PDF coming soon, video without decoder", "[providers][placeholder]") {
    test::TempDir dir;
    ProviderRegistry reg; // only the placeholders + text, no FFmpeg provider
    BuiltinProviderOptions opt;
    REQUIRE(register_builtin_providers(reg, opt));
    test::write_text(dir / "a.pdf", "%PDF-1.4");
    auto r = reg.produce(request(dir / "a.pdf", "application/pdf"));
    REQUIRE(r);
    CHECK(std::get<PlaceholderPreview>(r->content).reason == PlaceholderReason::ComingSoon);
    CHECK(r->provider_id == "placeholder.coming_soon");

    // An undecodable "video": FFmpeg (if built) says Unsupported, the placeholder follows.
    test::write_text(dir / "v.mp4", "not really a video");
    auto t = reg.produce(request(dir / "v.mp4", "video/mp4", PreviewPurpose::Thumbnail));
    if (t && t->provider_id == "placeholder.video") {
        CHECK(std::get<PlaceholderPreview>(t->content).reason == PlaceholderReason::NoThumbnailer);
        auto i = reg.produce(request(dir / "v.mp4", "video/mp4", PreviewPurpose::Inspector));
        REQUIRE(i);
        if (i->provider_id == "placeholder.video") {
            REQUIRE(i->kind() == PreviewKind::Video);
            CHECK(std::get<VideoPreview>(i->content).poster.empty());
            CHECK(std::get<VideoPreview>(i->content).info.source == MetadataSource::None);
        }
    } else {
        CHECK_FALSE(t); // FFmpeg reported the file corrupt
    }
}

TEST_CASE("External thumbnailer provider through a fake runner", "[providers][thumbnailer]") {
    test::TempDir dir;
    auto t = thumbnail::parse_thumbnailer(
        "[Thumbnailer Entry]\nExec=fake-thumb %i %o %s\nMimeType=image/tiff;video/x-fake;\n",
        "fake", "/f");
    REQUIRE(t);
    auto runner = std::make_shared<test::FakeRunner>();
    BuiltinProviderOptions opt;
    opt.thumbnailers = {*t};
    opt.runner = runner;
    auto reg = builtin(opt);
    REQUIRE(reg->find("thumbnailer.fake") != nullptr);
    test::write_text(dir / "a.tiff", "II*\0junk");
    auto req = request(dir / "a.tiff", "image/tiff", PreviewPurpose::Thumbnail);
    req.target = {64, 64};
    auto r = reg->produce(req);
    REQUIRE(r);
    CHECK(r->provider_id == "thumbnailer.fake");
    const auto& ip = std::get<ImagePreview>(r->content);
    CHECK(ip.origin == ImageOrigin::ExternalThumbnailer);
    CHECK(ip.image.size() == PixelSize{64, 43});
    CHECK(runner->requests().at(0).argv.at(3) == "128"); // class size for a 64 px box

    test::write_text(dir / "a.fake", "x");
    auto v = reg->produce(request(dir / "a.fake", "video/x-fake", PreviewPurpose::Inspector));
    REQUIRE(v);
    REQUIRE(v->kind() == PreviewKind::Video);
    CHECK(std::get<VideoPreview>(v->content).poster_origin == ImageOrigin::ExternalThumbnailer);
    CHECK(std::get<VideoPreview>(v->content).poster.size() == PixelSize{300, 200});
    CHECK(runner->requests().back().argv.at(3) == "1024");

    runner->mode = test::FakeRunner::Mode::ExitFailure;
    CHECK(reg->produce(req).error().code == ErrorCode::InvalidArgument);
}
