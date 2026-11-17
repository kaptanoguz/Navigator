// navigator::places JSON v1 and files: golden output, round trips with Turkish and non-UTF-8
// paths, lenient loading, the offline bookmark across a restart, size cap and ".bad" handling.
// Files live in a fresh temporary directory (temp_dir.hpp).
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/layout.hpp>
#include <navigator/places.hpp>

#include "places_test_util.hpp"
#include "temp_dir.hpp"

using namespace navigator;
using navigator::test::at_seconds;
using navigator::test::must;
using navigator::test::running_as_root;
using navigator::test::system_volumes;
using navigator::test::TempDir;
namespace pl = navigator::places;
namespace stdfs = std::filesystem;

namespace {

std::string read_all(const stdfs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_all(const stdfs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

pl::BookmarkStore must_bookmarks(const std::string& text) {
    Result<pl::BookmarkStore> store = pl::bookmarks_from_json(text);
    INFO(text);
    REQUIRE(store);
    return *store;
}

pl::RecentsStore must_recents(const std::string& text, std::size_t capacity = 50) {
    Result<pl::RecentsStore> store = pl::recents_from_json(text, capacity);
    INFO(text);
    REQUIRE(store);
    return *store;
}

} // namespace

TEST_CASE("places json: bookmarks golden output", "[places][json]") {
    pl::BookmarkStore store;
    const std::optional<VolumeIdentity> id =
        pl::identity_for("/media/u/STICK/İş", system_volumes());
    REQUIRE(store.add("/media/u/STICK/İş", id,
                      at_seconds(1731800000) + std::chrono::milliseconds(999), "Work"));
    REQUIRE(store.add(std::string("/raw/\xff"), std::nullopt, at_seconds(0)));
    VolumeIdentity nfs{"", "/", "server:/export", "/mnt/nfs"};
    REQUIRE(store.add("/mnt/nfs", nfs, at_seconds(5)));
    const Result<std::string> text = pl::bookmarks_to_json(store);
    REQUIRE(text);
    const std::string golden = R"({
  "version": 1,
  "bookmarks": [
    {
      "path": "/media/u/STICK/İş",
      "name": "Work",
      "added": 1731800000,
      "volume": {
        "uuid": "1234-ABCD",
        "root": "/",
        "device": "/dev/sdb1",
        "mount_point": "/media/u/STICK"
      }
    },
    {
      "path": {
        "hex": "2f7261772fff"
      },
      "added": 0
    },
    {
      "path": "/mnt/nfs",
      "added": 5,
      "volume": {
        "root": "/",
        "device": "server:/export",
        "mount_point": "/mnt/nfs"
      }
    }
  ]
}
)";
    CHECK(*text == golden);
}

TEST_CASE("places json: recents golden output", "[places][json]") {
    pl::RecentsStore store;
    REQUIRE(store.record("/home/u/Müzik", std::nullopt, at_seconds(10)));
    REQUIRE(store.record("/home/u/Müzik", std::nullopt, at_seconds(20)));
    REQUIRE(store.record("/tmp", std::nullopt, at_seconds(30)));
    pl::RecentsOptions options;
    options.highlight = true;
    store.set_options(options);
    const Result<std::string> text = pl::recents_to_json(store);
    REQUIRE(text);
    const std::string golden = R"({
  "version": 1,
  "options": {
    "enabled": true,
    "highlight": true,
    "sort_on_top": false
  },
  "recents": [
    {
      "path": "/tmp",
      "last_visit": 30,
      "visits": 1
    },
    {
      "path": "/home/u/Müzik",
      "last_visit": 20,
      "visits": 2
    }
  ]
}
)";
    CHECK(*text == golden);
}

TEST_CASE("places json: round trips keep Turkish and non-UTF-8 paths byte-exact",
          "[places][json]") {
    pl::BookmarkStore bookmarks;
    const std::vector<std::string> paths{"/home/u/Çalışma Alanı", "/home/u/İndirilenler",
                                         std::string("/data/\xc3\x28-latin\xe7"),
                                         "/home/u/ışık ĞÜŞÖÇ"};
    for (const std::string& p : paths) {
        REQUIRE(bookmarks.add(Path(p), pl::identity_for(Path(p), system_volumes()), at_seconds(42),
                              p.front() == '/' && p.size() > 20 ? "Şık" : ""));
    }
    const Result<std::string> btext = pl::bookmarks_to_json(bookmarks);
    REQUIRE(btext);
    const pl::BookmarkStore bback = must_bookmarks(*btext);
    REQUIRE(bback.size() == bookmarks.size());
    for (std::size_t i = 0; i < bback.size(); ++i) {
        CHECK(bback.list()[i].path.native() == paths[i]);
        CHECK(bback.list()[i].name == bookmarks.list()[i].name);
        CHECK(bback.list()[i].volume == bookmarks.list()[i].volume);
        CHECK(bback.list()[i].added == at_seconds(42));
        CHECK(bback.list()[i].id.valid());
    }
    CHECK(*pl::bookmarks_to_json(bback) == *btext);

    pl::RecentsStore recents(10);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        REQUIRE(recents.record(Path(paths[i]), pl::identity_for(Path(paths[i]), system_volumes()),
                               at_seconds(static_cast<long long>(100 + i))));
    }
    REQUIRE(recents.record(Path(paths[1]), std::nullopt, at_seconds(500)));
    pl::RecentsOptions options;
    options.enabled = false;
    options.sort_on_top = true;
    recents.set_options(options);
    const Result<std::string> rtext = pl::recents_to_json(recents);
    REQUIRE(rtext);
    const pl::RecentsStore rback = must_recents(*rtext, 10);
    REQUIRE(rback.size() == recents.size());
    for (std::size_t i = 0; i < rback.size(); ++i) {
        CHECK(rback.entries()[i] == recents.entries()[i]);
    }
    CHECK(rback.options() == options);
    CHECK(*pl::recents_to_json(rback) == *rtext);
}

TEST_CASE("places json: lenient bookmark loading", "[places][json]") {
    CHECK(pl::bookmarks_from_json("[]").error().code == ErrorCode::InvalidArgument);
    CHECK(pl::bookmarks_from_json("{").error().code == ErrorCode::InvalidArgument);
    CHECK(pl::bookmarks_from_json("").error().code == ErrorCode::InvalidArgument);
    CHECK(must_bookmarks("{}").empty());
    CHECK(must_bookmarks(R"({"version": 99, "bookmarks": {"a": 1}})").empty());

    const pl::BookmarkStore s = must_bookmarks(R"({"version": "x", "future": [1, 2], "bookmarks": [
        {"path": "/a//b/", "name": "Bee", "added": 7, "extra": true},
        {"path": "relative"}, {"path": ""}, {"nopath": 1}, 5, "str", {"path": 12},
        {"path": "/a/b", "name": "duplicate"},
        {"path": "/c", "name": "bad\u0001name", "added": 1.5},
        {"path": {"hex": "2f64ff"}, "name": 3, "added": "soon",
         "volume": {"root": "/", "mount_point": "/m"}},
        {"path": "/e", "volume": {"device": "/dev/sdz1", "mount_point": {"hex": "2f6d"}}},
        {"path": "/f", "volume": "nope"}
    ]})");
    REQUIRE(s.size() == 5);
    CHECK(s.list()[0].path == "/a/b");
    CHECK(s.list()[0].name == "Bee");
    CHECK(s.list()[0].added == at_seconds(7));
    CHECK(s.list()[1].path == "/c");
    CHECK(s.list()[1].name.empty());
    CHECK(s.list()[1].added == Timestamp{});
    CHECK(s.list()[2].path.native() == std::string("/d\xff"));
    CHECK(s.list()[2].name.empty());
    CHECK_FALSE(s.list()[2].volume.has_value()); // neither uuid nor device: absent
    REQUIRE(s.list()[3].volume.has_value());
    CHECK(must(s.list()[3].volume).device == "/dev/sdz1");
    CHECK(must(s.list()[3].volume).mount_point == "/m");
    CHECK_FALSE(s.list()[4].volume.has_value());

    // Huge timestamps saturate instead of overflowing.
    const pl::BookmarkStore big =
        must_bookmarks(R"({"bookmarks": [{"path": "/x", "added": 9223372036854775807}]})");
    CHECK(big.list()[0].added > at_seconds(0));

    // Beyond kMaxBookmarks are dropped.
    std::string many = R"({"bookmarks": [)";
    for (std::size_t i = 0; i < pl::kMaxBookmarks + 5; ++i) {
        many += (i > 0 ? "," : "") + std::string(R"({"path": "/p)") + std::to_string(i) + "\"}";
    }
    many += "]}";
    const pl::BookmarkStore capped = must_bookmarks(many);
    CHECK(capped.size() == pl::kMaxBookmarks);
    CHECK(capped.list().back().path == Path("/p" + std::to_string(pl::kMaxBookmarks - 1)));
}

TEST_CASE("places json: lenient recents loading", "[places][json]") {
    CHECK(pl::recents_from_json("null").error().code == ErrorCode::InvalidArgument);
    const pl::RecentsStore empty = must_recents("{}");
    CHECK(empty.empty());
    CHECK(empty.options() == pl::RecentsOptions{});

    const pl::RecentsStore s = must_recents(R"({"options": {"enabled": "no", "highlight": true},
        "recents": [
          {"path": "/a", "visits": 0, "last_visit": 3},
          {"path": "/b/", "visits": -4},
          {"path": "/c", "visits": 1.0},
          {"path": "/d", "visits": 99999999999},
          {"path": "/a", "visits": 7},
          {"path": "x"},
          {"path": "/e", "visits": 5}
        ]})",
                                            4);
    CHECK(s.options().enabled);
    CHECK(s.options().highlight);
    CHECK(s.capacity() == 4);
    REQUIRE(s.size() == 4); // capacity: "/e" dropped
    CHECK(s.entries()[0].path == "/a");
    CHECK(s.entries()[0].visit_count == 1);
    CHECK(s.entries()[0].last_visit == at_seconds(3));
    CHECK(s.entries()[1].path == "/b");
    CHECK(s.entries()[1].visit_count == 1);
    CHECK(s.entries()[2].visit_count == 1);
    CHECK(s.entries()[3].visit_count == 4294967295U);
    CHECK(must_recents(R"({"options": [], "recents": 5})").empty());
}

TEST_CASE("places files: acceptance -- an offline bookmark survives a restart",
          "[places][files][acceptance]") {
    TempDir tmp;
    const Path file = pl::bookmarks_file(tmp.path() / "navigator");
    {
        pl::BookmarkStore store;
        REQUIRE(store.add("/media/u/STICK/Arşiv",
                          pl::identity_for("/media/u/STICK/Arşiv", system_volumes()),
                          at_seconds(1)));
        REQUIRE(pl::save_bookmarks(file, store));
    }
    std::vector<VolumeInfo> unplugged = system_volumes();
    unplugged.pop_back();
    {
        Result<pl::BookmarkStore> loaded = pl::load_bookmarks(file);
        REQUIRE(loaded);
        REQUIRE(loaded->size() == 1);
        const pl::Bookmark& b = loaded->list()[0];
        CHECK(pl::resolve_location(b.path, b.volume, unplugged).status ==
              pl::LocationStatus::Offline);
        CHECK(loaded->resync(unplugged) == 0);
        REQUIRE(pl::save_bookmarks(file, *loaded)); // saved while offline: still kept
    }
    std::vector<VolumeInfo> moved = system_volumes();
    moved.back().mount_point = "/run/media/u/STICK";
    Result<pl::BookmarkStore> again = pl::load_bookmarks(file);
    REQUIRE(again);
    REQUIRE(again->size() == 1);
    CHECK(again->resync(moved) == 1);
    const pl::Bookmark& b = again->list()[0];
    const pl::ResolvedLocation r = pl::resolve_location(b.path, b.volume, moved);
    CHECK(r.status == pl::LocationStatus::Online);
    CHECK(r.path == "/run/media/u/STICK/Arşiv");
}

TEST_CASE("places files: save/load, permissions, NotFound, .bad, size cap", "[places][files]") {
    TempDir tmp;
    const Path root = tmp.path() / "cfg" / "navigator";
    const Path bfile = pl::bookmarks_file(root);
    const Path rfile = pl::recents_file(root);
    CHECK(bfile.filename() == "bookmarks.json");
    CHECK(rfile.filename() == "recents.json");

    CHECK(pl::load_bookmarks(bfile).error().code == ErrorCode::NotFound);
    CHECK(pl::load_recents(rfile).error().code == ErrorCode::NotFound);
    CHECK(pl::load_bookmarks("").error().code == ErrorCode::InvalidArgument);
    CHECK(pl::save_bookmarks("", {}).error().code == ErrorCode::InvalidArgument);
    CHECK(pl::save_recents("", {}).error().code == ErrorCode::InvalidArgument);

    pl::BookmarkStore bookmarks;
    REQUIRE(bookmarks.add("/home/u/Belgeler", std::nullopt, at_seconds(3), "Belgeler"));
    REQUIRE(pl::save_bookmarks(bfile, bookmarks));
    pl::RecentsStore recents;
    REQUIRE(recents.record("/srv", std::nullopt, at_seconds(4)));
    REQUIRE(pl::save_recents(rfile, recents));

    struct stat st {};
    REQUIRE(::stat(bfile.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    REQUIRE(::stat(root.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    std::size_t files = 0;
    for (const auto& e : stdfs::directory_iterator(root)) {
        (void)e;
        ++files;
    }
    CHECK(files == 2); // no temporary files left

    Result<pl::BookmarkStore> bl = pl::load_bookmarks(bfile);
    REQUIRE(bl);
    CHECK(bl->list()[0].name == "Belgeler");
    Result<pl::RecentsStore> rl = pl::load_recents(rfile, 7);
    REQUIRE(rl);
    CHECK(rl->capacity() == 7);
    CHECK(rl->entries()[0].path == "/srv");

    // Not an object: moved aside to .bad (replacing an older one).
    write_all(stdfs::path(root.native() + "/bookmarks.json.bad"), "old");
    write_all(bfile, "[1, 2]");
    const Result<pl::BookmarkStore> bad = pl::load_bookmarks(bfile);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(stdfs::exists(bfile));
    CHECK(read_all(bfile.native() + ".bad") == "[1, 2]");
    write_all(rfile, "garbage{");
    CHECK(pl::load_recents(rfile).error().code == ErrorCode::InvalidArgument);
    CHECK(stdfs::exists(rfile.native() + ".bad"));

    // Larger than kMaxDocumentBytes: rejected without parsing, left in place.
    {
        std::ofstream out(bfile, std::ios::binary);
        out << R"({"bookmarks": []})" << std::string(layout::kMaxDocumentBytes, ' ');
    }
    CHECK(pl::load_bookmarks(bfile).error().code == ErrorCode::InvalidArgument);
    CHECK(stdfs::exists(bfile));

    if (!running_as_root()) {
        ::chmod(root.c_str(), 0500);
        CHECK(pl::save_recents(rfile.parent_path() / "new.json", recents).error().code ==
              ErrorCode::PermissionDenied);
        ::chmod(root.c_str(), 0700);
    }
}
