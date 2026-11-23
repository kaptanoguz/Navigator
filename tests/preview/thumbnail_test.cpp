// tests/preview/thumbnail_test.cpp: Freedesktop thumbnail cache: GLib-exact URIs (reference
// strings produced by GLib.filename_to_uri), MD5 names, paths, policy, Valid / Stale / Missing /
// Invalid, write then read back, permissions, failure markers. Temporary cache roots only.
#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/thumbnail.hpp>

#include "test_util.hpp"

using namespace navigator;
using namespace navigator::thumbnail;
using preview::ImageBuffer;
using preview::PixelSize;

namespace {

std::string uri(const char* p) {
    auto u = file_uri(p);
    REQUIRE(u);
    return *u;
}

ImageBuffer solid(std::uint32_t w, std::uint32_t h, std::array<std::uint8_t, 4> c) {
    auto img = preview::make_image({w, h});
    REQUIRE(img);
    for (std::size_t i = 0; i < img->pixels.size(); i += 4) {
        std::memcpy(img->pixels.data() + i, c.data(), 4);
    }
    return std::move(*img);
}

SourceInfo source_for(const std::filesystem::path& p, std::int64_t mtime = 1700000000,
                      std::uint64_t size = 1234) {
    SourceInfo s;
    s.path = p;
    s.uri = *file_uri(p);
    s.mtime = Timestamp{std::chrono::seconds(mtime)} + std::chrono::milliseconds(250);
    s.size = size;
    s.mime = "image/png";
    return s;
}

std::uint32_t mode_of(const std::filesystem::path& p) {
    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return st.st_mode & 07777U;
}

} // namespace

TEST_CASE("file_uri matches GLib g_filename_to_uri", "[thumbnail][uri]") {
    // Reference strings from GLib.filename_to_uri (GLib 2.80).
    CHECK(uri("/home/oğuz/a b.jpg") == "file:///home/o%C4%9Fuz/a%20b.jpg");
    CHECK(uri("/tmp/100%.png") == "file:///tmp/100%25.png");
    CHECK(uri("/tmp/a#b?c;d[e]f{g}h|i^j`k\"l<m>n\\o") ==
          "file:///tmp/a%23b%3Fc%3Bd%5Be%5Df%7Bg%7Dh%7Ci%5Ej%60k%22l%3Cm%3En%5Co");
    CHECK(uri("/tmp/~!$&'()*+,:=@-._x") == "file:///tmp/~!$&'()*+,:=@-._x");
    CHECK(uri("/tmp/tab\there") == "file:///tmp/tab%09here");
    CHECK(uri("/tmp/ü/日本.png") == "file:///tmp/%C3%BC/%E6%97%A5%E6%9C%AC.png");
    CHECK(uri("/tmp/\xFF\x01raw") == "file:///tmp/%FF%01raw");
    CHECK(uri("/tmp/./x/../y.png") == "file:///tmp/y.png"); // lexically normal
    CHECK(uri("/") == "file:///");
    CHECK_FALSE(file_uri("relative/x.png"));
    CHECK(file_uri("").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("md5_hex and thumbnail names", "[thumbnail][md5]") {
    CHECK(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5_hex("The quick brown fox jumps over the lazy dog") ==
          "9e107d9d372bb6826bd81d3542a419d6");
    CHECK(md5_hex(std::string(1000, 'a')) == "cabe45dcc9ae5b66ba86600cca6b8ba8");
    // Example from the Thumbnail Managing Standard.
    CHECK(thumbnail_file_name("file:///home/jens/photos/me.png") ==
          "c6ee772d9e49320e97ec29a7eb5b1697.png");
    CHECK(thumbnail_path("/c", ThumbnailSize::Large, "file:///home/jens/photos/me.png") ==
          std::filesystem::path("/c/large/c6ee772d9e49320e97ec29a7eb5b1697.png"));
    CHECK(failure_path("/c", "file:///home/jens/photos/me.png") ==
          std::filesystem::path("/c/fail/navigator-1/c6ee772d9e49320e97ec29a7eb5b1697.png"));
    CHECK(cache_root_for("/home/u/.cache") == std::filesystem::path("/home/u/.cache/thumbnails"));
}

TEST_CASE("size classes, is_inside_cache and decide()", "[thumbnail][policy]") {
    CHECK(size_for(48) == ThumbnailSize::Normal);
    CHECK(size_for(128) == ThumbnailSize::Normal);
    CHECK(size_for(129) == ThumbnailSize::Large);
    CHECK(size_for(600) == ThumbnailSize::XXLarge);
    CHECK(size_for(5000) == ThumbnailSize::XXLarge);
    CHECK(dir_name(ThumbnailSize::XLarge) == "x-large");

    const std::filesystem::path root = "/home/u/.cache/thumbnails";
    CHECK(is_inside_cache("/home/u/.cache/thumbnails/normal/x.png", root));
    CHECK(is_inside_cache("/home/u/.cache/thumbnails", root));
    CHECK(is_inside_cache("/home/u/.cache/thumbnails/../thumbnails/large/a.png", root));
    CHECK_FALSE(is_inside_cache("/home/u/.cache/thumbnails2/x.png", root));
    CHECK_FALSE(is_inside_cache("/home/u/Pictures/x.png", root));
    CHECK(is_inside_cache("/home/u/.cache/thumbnails/x.png", "/home/u/.cache/thumbnails/"));

    fs::DirEntry file;
    file.kind = fs::EntryKind::File;
    const preview::SourceLocation local;
    const Policy policy;
    CHECK(decide("/home/u/a.png", file, local, root, policy) == Decision::Generate);
    CHECK(decide("a.png", file, local, root, policy) == Decision::Skip);
    CHECK(decide("", file, local, root, policy) == Decision::Skip);
    CHECK(decide("/home/u/.cache/thumbnails/normal/a.png", file, local, root, policy) ==
          Decision::Skip);
    fs::DirEntry folder;
    folder.kind = fs::EntryKind::Directory;
    CHECK(decide("/home/u/d", folder, local, root, policy) == Decision::Skip);
    fs::DirEntry link;
    link.kind = fs::EntryKind::Symlink;
    link.target_kind = fs::EntryKind::File;
    CHECK(decide("/home/u/l.png", link, local, root, policy) == Decision::Generate);
    link.dangling = true;
    link.target_kind = fs::EntryKind::Unknown;
    CHECK(decide("/home/u/l.png", link, local, root, policy) == Decision::Skip);
    fs::DirEntry fifo;
    fifo.kind = fs::EntryKind::Other;
    CHECK(decide("/home/u/f", fifo, local, root, policy) == Decision::Skip);
    preview::SourceLocation net;
    net.network = true;
    CHECK(decide("/mnt/n/a.png", file, net, root, policy) == Decision::ReadOnly);
    Policy on;
    on.generate_on_network = true;
    CHECK(decide("/mnt/n/a.png", file, net, root, on) == Decision::Generate);
    preview::SourceLocation usb;
    usb.removable = true;
    CHECK(decide("/media/a.png", file, usb, root, policy) == Decision::ReadOnly);
    on.generate_on_removable = true;
    CHECK(decide("/media/a.png", file, usb, root, on) == Decision::Generate);
}

TEST_CASE("write_thumbnail then read back; Valid, Stale, Missing, Invalid", "[thumbnail][io]") {
    test::TempDir tmp;
    const auto root = tmp / "cache" / "thumbnails"; // missing parents created 0700
    const SourceInfo src = source_for(tmp / "photo 1.jpg");
    const ImageBuffer img = solid(128, 96, {64, 32, 16, 128}); // premultiplied
    WriteOptions wo;
    wo.original = {4000, 3000};
    auto written = write_thumbnail(root, ThumbnailSize::Normal, src, img, wo);
    REQUIRE(written);
    CHECK(*written == thumbnail_path(root, ThumbnailSize::Normal, src.uri));
    CHECK(mode_of(*written) == 0600U);
    CHECK(mode_of(root / "normal") == 0700U);
    CHECK(mode_of(root) == 0700U);
    // No temporary files left behind.
    std::size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(root / "normal")) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);

    auto r = read_thumbnail(*written, src, {});
    REQUIRE(r);
    CHECK(r->status == LookupStatus::Valid);
    CHECK(r->original == PixelSize{4000, 3000});
    REQUIRE(r->image.size() == PixelSize{128, 96});
    const auto px = test::pixel(r->image, 5, 5);
    CHECK(test::near(px, {64, 32, 16, 128}, 1)); // straight on disk, premultiplied again
    CHECK_FALSE(r->movie_length_s);

    // Thumb::Size and Thumb::MTime mismatch -> Stale; only whole seconds count.
    SourceInfo later = src;
    later.mtime += std::chrono::seconds(1);
    CHECK(read_thumbnail(*written, later, {})->status == LookupStatus::Stale);
    SourceInfo sub = src;
    sub.mtime += std::chrono::milliseconds(500);
    CHECK(read_thumbnail(*written, sub, {})->status == LookupStatus::Valid);
    SourceInfo bigger = src;
    bigger.size = 99;
    CHECK(read_thumbnail(*written, bigger, {})->status == LookupStatus::Stale);
    // Wrong URI -> Invalid.
    SourceInfo other = source_for(tmp / "other.jpg");
    CHECK(read_thumbnail(*written, other, {})->status == LookupStatus::Invalid);
    // Missing file.
    CHECK(read_thumbnail(tmp / "nope.png", src, {})->status == LookupStatus::Missing);
    // Not a PNG / truncated.
    test::write_text(tmp / "junk.png", "not a png at all");
    CHECK(read_thumbnail(tmp / "junk.png", src, {})->status == LookupStatus::Invalid);
    auto bytes = test::read_file(*written);
    bytes.resize(bytes.size() - 30);
    test::write_file(tmp / "trunc.png", bytes);
    CHECK(read_thumbnail(tmp / "trunc.png", src, {})->status == LookupStatus::Invalid);
    // A PNG without Thumb::MTime.
    test::PngOptions noms;
    noms.text = {{"Thumb::URI", src.uri}};
    test::write_file(tmp / "nomtime.png", test::encode_png(test::quadrants(8, 8), noms));
    CHECK(read_thumbnail(tmp / "nomtime.png", src, {})->status == LookupStatus::Invalid);
    // Another program's thumbnail (no Thumb::Size, float MTime) is reused.
    test::PngOptions gnome;
    gnome.text = {
        {"Thumb::URI", src.uri}, {"Thumb::MTime", "1700000000"}, {"Thumb::Movie::Length", "42"}};
    test::write_file(tmp / "gnome.png", test::encode_png(test::quadrants(8, 8), gnome));
    auto g = read_thumbnail(tmp / "gnome.png", src, {});
    REQUIRE(g);
    CHECK(g->status == LookupStatus::Valid);
    CHECK(g->movie_length_s == std::int64_t{42});
    // Oversized dimensions are Invalid without decoding.
    test::write_file(tmp / "huge.png", test::png_ihdr_only(5000, 5000));
    CHECK(read_thumbnail(tmp / "huge.png", src, {})->status == LookupStatus::Invalid);
}

TEST_CASE("lookup_thumbnail tries larger classes and aggregates status", "[thumbnail][io]") {
    test::TempDir tmp;
    const auto root = tmp / "thumbnails";
    const SourceInfo src = source_for(tmp / "a.png");
    auto miss = lookup_thumbnail(root, ThumbnailSize::Normal, src, {});
    REQUIRE(miss);
    CHECK(miss->status == LookupStatus::Missing);
    CHECK(miss->file == thumbnail_path(root, ThumbnailSize::Normal, src.uri));

    REQUIRE(write_thumbnail(root, ThumbnailSize::XLarge, src, solid(512, 256, {1, 2, 3, 255}), {}));
    auto hit = lookup_thumbnail(root, ThumbnailSize::Normal, src, {});
    REQUIRE(hit);
    CHECK(hit->status == LookupStatus::Valid);
    CHECK(hit->size_class == ThumbnailSize::XLarge);
    CHECK(lookup_thumbnail(root, ThumbnailSize::XXLarge, src, {})->status == LookupStatus::Missing);

    SourceInfo changed = src;
    changed.mtime += std::chrono::seconds(10);
    CHECK(lookup_thumbnail(root, ThumbnailSize::Normal, changed, {})->status ==
          LookupStatus::Stale);

    std::filesystem::create_directories(root / "normal");
    test::write_text(thumbnail_path(root, ThumbnailSize::Normal, src.uri), "garbage");
    std::filesystem::remove(thumbnail_path(root, ThumbnailSize::XLarge, src.uri));
    CHECK(lookup_thumbnail(root, ThumbnailSize::Normal, src, {})->status == LookupStatus::Invalid);
}

TEST_CASE("write_thumbnail rejects bad input and files inside the cache", "[thumbnail][io]") {
    test::TempDir tmp;
    const auto root = tmp / "thumbnails";
    const SourceInfo src = source_for(tmp / "a.png");
    CHECK(write_thumbnail(root, ThumbnailSize::Normal, src, solid(129, 10, {0, 0, 0, 255}), {})
              .error()
              .code == ErrorCode::InvalidArgument);
    CHECK(write_thumbnail(root, ThumbnailSize::Normal, src, ImageBuffer{}, {}).error().code ==
          ErrorCode::InvalidArgument);
    const SourceInfo inside = source_for(root / "normal" / "x.png");
    CHECK(write_thumbnail(root, ThumbnailSize::Normal, inside, solid(8, 8, {0, 0, 0, 255}), {})
              .error()
              .code == ErrorCode::InvalidArgument);
    CHECK_FALSE(std::filesystem::exists(root));
    if (::geteuid() != 0) {
        std::filesystem::create_directories(tmp / "ro");
        ::chmod((tmp / "ro").c_str(), 0500);
        auto r = write_thumbnail(tmp / "ro" / "t", ThumbnailSize::Normal, src,
                                 solid(8, 8, {0, 0, 0, 255}), {});
        CHECK(r.error().code == ErrorCode::PermissionDenied);
        ::chmod((tmp / "ro").c_str(), 0700);
    }
}

TEST_CASE("failure markers are ours, validated by URI and MTime", "[thumbnail][fail]") {
    test::TempDir tmp;
    const auto root = tmp / "thumbnails";
    const SourceInfo src = source_for(tmp / "broken.jpg");
    CHECK_FALSE(*has_failure(root, src));
    REQUIRE(write_failure(root, src));
    CHECK(std::filesystem::exists(failure_path(root, src.uri)));
    CHECK(mode_of(failure_path(root, src.uri)) == 0600U);
    CHECK(*has_failure(root, src));
    SourceInfo changed = src;
    changed.mtime += std::chrono::seconds(5);
    CHECK_FALSE(*has_failure(root, changed)); // a changed file is retried
    SourceInfo bigger = src;
    bigger.size = 1;
    CHECK(*has_failure(root, bigger)); // markers carry no Thumb::Size
    // Other programs' fail dirs are never read.
    std::filesystem::create_directories(root / "fail" / "gnome-thumbnail-factory");
    std::filesystem::copy_file(failure_path(root, src.uri), root / "fail" /
                                                                "gnome-thumbnail-factory" /
                                                                thumbnail_file_name(src.uri));
    std::filesystem::remove(failure_path(root, src.uri));
    CHECK_FALSE(*has_failure(root, src));
    // Corrupt marker counts as absent.
    std::filesystem::create_directories(failure_path(root, src.uri).parent_path());
    test::write_text(failure_path(root, src.uri), "xx");
    CHECK_FALSE(*has_failure(root, src));
}
