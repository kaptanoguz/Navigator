// navigator::places: location resolution and rebasing against fake volume tables, untracked
// volumes, highlight helper, labels, the Places list, and the bookmark / recents stores.
// Pure: no filesystem access.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/places.hpp>

#include "places_test_util.hpp"

using namespace navigator;
using navigator::test::at_seconds;
using navigator::test::must;
using navigator::test::stick;
using navigator::test::system_volumes;
using navigator::test::volume;
namespace pl = navigator::places;

TEST_CASE("places: containing_volume picks the longest component-wise mount prefix", "[places]") {
    const std::vector<VolumeInfo> vols = system_volumes();
    CHECK(pl::containing_volume("/etc", vols) == 0);
    CHECK(pl::containing_volume("/", vols) == 0);
    CHECK(pl::containing_volume("/home/u", vols) == 1);
    CHECK(pl::containing_volume("/home/u/Belgeler", vols) == 1);
    CHECK(pl::containing_volume("/home/user2", vols) == 0); // "/home/u" is not a prefix
    CHECK(pl::containing_volume("/media/u/STICK/a/b", vols) == 2);
    CHECK(pl::containing_volume("/media/u/STICK2", vols) == 0);
    CHECK(pl::containing_volume("/media/u/STICK/", vols) == 2);
    CHECK_FALSE(pl::containing_volume("relative/x", vols).has_value());
    CHECK_FALSE(pl::containing_volume("", vols).has_value());
    CHECK_FALSE(pl::containing_volume("/x", {}).has_value());
}

TEST_CASE("places: identity_for records only tracked volumes", "[places]") {
    std::vector<VolumeInfo> vols = system_volumes();
    // Root and Home are never recorded (their UUID changes on reinstall).
    CHECK_FALSE(pl::identity_for("/etc", vols).has_value());
    CHECK_FALSE(pl::identity_for("/home/u/Müzik", vols).has_value());
    const std::optional<VolumeIdentity> id = pl::identity_for("/media/u/STICK/docs", vols);
    REQUIRE(id.has_value());
    CHECK(must(id).uuid == "1234-ABCD");
    CHECK(must(id).mount_point == "/media/u/STICK");
    CHECK(must(id).device == "/dev/sdb1");
    CHECK(must(id).root == "/");

    VolumeInfo proc = volume("/proc", "proc", std::nullopt, VolumeKind::Virtual);
    proc.device = "proc";
    vols.push_back(proc);
    CHECK_FALSE(pl::identity_for("/proc/1", vols).has_value());
    CHECK_FALSE(pl::is_tracked_volume(proc));
    CHECK(pl::is_tracked_volume(stick("/media/u/STICK")));

    // A tracked volume without uuid and device has nothing to recognise it by.
    VolumeInfo anon = volume("/mnt/anon", "", std::nullopt, VolumeKind::Other);
    vols.push_back(anon);
    CHECK_FALSE(pl::identity_for("/mnt/anon/x", vols).has_value());

    // Network shares keep the device fallback.
    VolumeInfo nfs = volume("/mnt/nfs", "server:/export", std::nullopt, VolumeKind::Network);
    vols.push_back(nfs);
    const std::optional<VolumeIdentity> net = pl::identity_for("/mnt/nfs/a", vols);
    REQUIRE(net.has_value());
    CHECK(must(net).uuid.empty());
    CHECK(must(net).device == "server:/export");
}

TEST_CASE("places: acceptance -- a bookmark on an unmounted volume stays and is Offline",
          "[places][acceptance]") {
    std::vector<VolumeInfo> mounted = system_volumes();
    pl::BookmarkStore store;
    const Path where = "/media/u/STICK/Proje Ödevleri";
    const std::optional<VolumeIdentity> id = pl::identity_for(where, mounted);
    REQUIRE(id.has_value());
    const Result<pl::BookmarkId> added = store.add(where, id, at_seconds(100), "Ödev");
    REQUIRE(added);

    // Unmounted: the store keeps it; resolution says Offline with the stored path.
    std::vector<VolumeInfo> unplugged = system_volumes();
    unplugged.pop_back();
    CHECK(store.resync(unplugged) == 0);
    REQUIRE(store.size() == 1);
    const pl::Bookmark& b = store.list()[0];
    pl::ResolvedLocation r = pl::resolve_location(b.path, b.volume, unplugged);
    CHECK(r.status == pl::LocationStatus::Offline);
    CHECK(r.path == where);
    CHECK_FALSE(r.volume_index.has_value());

    // Another disk at the same mount point is the wrong disk: still Offline.
    std::vector<VolumeInfo> impostor = unplugged;
    VolumeInfo other = stick("/media/u/STICK");
    other.uuid = "FFFF-0000";
    other.device = "/dev/sdb1";
    impostor.push_back(other);
    CHECK(pl::resolve_location(b.path, b.volume, impostor).status == pl::LocationStatus::Offline);
    CHECK(store.resync(impostor) == 0);

    // Back at the same place: Online, path unchanged, index of the volume.
    r = pl::resolve_location(b.path, b.volume, mounted);
    CHECK(r.status == pl::LocationStatus::Online);
    CHECK(r.path == where);
    CHECK(r.volume_index == 2);
    CHECK(store.resync(mounted) == 0);
}

TEST_CASE("places: acceptance -- a volume returning at another mount point is rebased",
          "[places][acceptance]") {
    pl::BookmarkStore store;
    const std::optional<VolumeIdentity> id =
        pl::identity_for("/media/u/STICK/a/b", system_volumes());
    REQUIRE(store.add("/media/u/STICK/a/b", id, at_seconds(1)));
    REQUIRE(store.add("/media/u/STICK", id, at_seconds(2)));

    std::vector<VolumeInfo> moved = system_volumes();
    moved.back().mount_point = "/media/u/STICK1";
    moved.back().device = "/dev/sdc1";

    const pl::Bookmark first = store.list()[0];
    const pl::ResolvedLocation r = pl::resolve_location(first.path, first.volume, moved);
    CHECK(r.status == pl::LocationStatus::Online);
    CHECK(r.path == "/media/u/STICK1/a/b");
    CHECK(r.volume_index == 2);
    // Resolution is pure: the store is untouched until resync.
    CHECK(store.list()[0].path == "/media/u/STICK/a/b");

    CHECK(store.resync(moved) == 2);
    CHECK(store.list()[0].path == "/media/u/STICK1/a/b");
    CHECK(store.list()[1].path == "/media/u/STICK1");
    REQUIRE(store.list()[0].volume.has_value());
    CHECK(must(store.list()[0].volume).mount_point == "/media/u/STICK1");
    CHECK(must(store.list()[0].volume).device == "/dev/sdc1");
    CHECK(must(store.list()[0].volume).uuid == "1234-ABCD");
    // Idempotent.
    CHECK(store.resync(moved) == 0);
    // And back again.
    CHECK(store.resync(system_volumes()) == 2);
    CHECK(store.list()[0].path == "/media/u/STICK/a/b");
}

TEST_CASE("places: resync never duplicates a bookmark path", "[places]") {
    pl::BookmarkStore store;
    const std::optional<VolumeIdentity> id = pl::identity_for("/media/u/STICK/x", system_volumes());
    REQUIRE(store.add("/media/u/STICK/x", id, at_seconds(1)));
    REQUIRE(store.add("/media/u/STICK1/x", std::nullopt, at_seconds(2)));
    std::vector<VolumeInfo> moved = system_volumes();
    moved.back().mount_point = "/media/u/STICK1";
    CHECK(store.resync(moved) == 0);
    CHECK(store.list()[0].path == "/media/u/STICK/x");
    CHECK(must(store.list()[0].volume).mount_point == "/media/u/STICK");
}

TEST_CASE("places: btrfs subvolumes are told apart by root", "[places]") {
    VolumeInfo top = volume("/mnt/pool", "/dev/sda2", std::string("UUID-B"), VolumeKind::Internal);
    top.root = "/@data";
    VolumeInfo snap = top;
    snap.mount_point = "/mnt/snap";
    snap.root = "/@snap";
    const std::vector<VolumeInfo> vols{snap, top};
    const std::optional<VolumeIdentity> id = pl::identity_for("/mnt/pool/x", vols);
    REQUIRE(id.has_value());
    const pl::ResolvedLocation r = pl::resolve_location("/mnt/pool/x", id, vols);
    CHECK(r.status == pl::LocationStatus::Online);
    CHECK(r.volume_index == 1);
    CHECK(r.path == "/mnt/pool/x");
}

TEST_CASE("places: device-only identities match only at the same mount point", "[places]") {
    const VolumeInfo nfs = volume("/mnt/nfs", "server:/export", std::nullopt, VolumeKind::Network);
    const std::optional<VolumeIdentity> id = pl::identity_for("/mnt/nfs/a", {&nfs, 1});
    REQUIRE(id.has_value());
    VolumeInfo elsewhere = nfs;
    elsewhere.mount_point = "/mnt/other";
    CHECK(pl::resolve_location("/mnt/nfs/a", id, {&elsewhere, 1}).status ==
          pl::LocationStatus::Offline);
    CHECK(pl::resolve_location("/mnt/nfs/a", id, {&nfs, 1}).status == pl::LocationStatus::Online);
}

TEST_CASE("places: untracked locations (Root/Home/Virtual or none) are always Online", "[places]") {
    const std::vector<VolumeInfo> vols = system_volumes();
    pl::ResolvedLocation r = pl::resolve_location("/home/u/Belgeler", std::nullopt, vols);
    CHECK(r.status == pl::LocationStatus::Online);
    CHECK(r.path == "/home/u/Belgeler");
    CHECK(r.volume_index == 1);
    r = pl::resolve_location("/srv", std::nullopt, {});
    CHECK(r.status == pl::LocationStatus::Online);
    CHECK_FALSE(r.volume_index.has_value());
    // An identity without uuid and device is treated as absent (volume.hpp).
    r = pl::resolve_location("/srv", VolumeIdentity{}, vols);
    CHECK(r.status == pl::LocationStatus::Online);
    CHECK(r.volume_index == 0);
    // A store never records such an identity.
    pl::BookmarkStore store;
    REQUIRE(store.add("/srv", VolumeIdentity{"", "/", "", "/srv"}, at_seconds(0)));
    CHECK_FALSE(store.list()[0].volume.has_value());
}

TEST_CASE("places: deepest_containing highlights the deepest row, ties to the lowest index",
          "[places]") {
    const std::vector<Path> rows{"/", "/home/u", "/home/u/İndirilenler", "/home/u", "/media/x"};
    CHECK(pl::deepest_containing("/home/u/İndirilenler/a", rows) == 2);
    CHECK(pl::deepest_containing("/home/u", rows) == 1);
    CHECK(pl::deepest_containing("/home/u/", rows) == 1);
    CHECK(pl::deepest_containing("/home/user", rows) == 0);
    CHECK(pl::deepest_containing("/etc", rows) == 0);
    CHECK_FALSE(pl::deepest_containing("/etc", std::vector<Path>{"/home"}).has_value());
    CHECK_FALSE(pl::deepest_containing("rel", rows).has_value());
}

TEST_CASE("places: default_label", "[places]") {
    CHECK(pl::default_label("/") == "/");
    CHECK(pl::default_label("/home/u/Çalışma") == "Çalışma");
    CHECK(pl::default_label("/home/u/x/") == "x");
    CHECK(pl::default_label(std::string("/raw/a\xff")) == "a\xEF\xBF\xBD");
    CHECK(pl::default_label("").empty());
}

TEST_CASE("places: build_places order, disabled and duplicate dirs", "[places]") {
    platform::UserDirs dirs;
    dirs.desktop = "/home/u/Masaüstü";
    dirs.documents = "/home/u/Belgeler/";
    dirs.downloads = "/home/u/İndirilenler";
    dirs.music = "/home/u";               // xdg "disabled"
    dirs.pictures = "relative";           // ignored
    dirs.videos = "/home/u/Belgeler";     // duplicate of Documents
    dirs.templates = "/home/u/Şablonlar"; // never shown
    const std::vector<pl::Place> places = pl::build_places("/home/u", dirs);
    REQUIRE(places.size() == 5);
    CHECK(places[0].kind == pl::PlaceKind::Home);
    CHECK(places[0].path == "/home/u");
    CHECK(places[1].kind == pl::PlaceKind::Desktop);
    CHECK(places[2].kind == pl::PlaceKind::Documents);
    CHECK(places[2].path == "/home/u/Belgeler");
    CHECK(places[3].kind == pl::PlaceKind::Downloads);
    CHECK(places[4].kind == pl::PlaceKind::FileSystem);
    CHECK(places[4].path == "/");
    CHECK(pl::label_key(places[4].kind) == "place.file_system");

    const std::vector<pl::Place> no_home = pl::build_places("", {});
    REQUIRE(no_home.size() == 1);
    CHECK(no_home[0].kind == pl::PlaceKind::FileSystem);
}

TEST_CASE("places: BookmarkStore edits", "[places][bookmarks]") {
    pl::BookmarkStore store;
    const pl::BookmarkId a = *store.add("/a//b/./", std::nullopt, at_seconds(1));
    const pl::BookmarkId b = *store.add("/c", std::nullopt, at_seconds(2), "Cee");
    const pl::BookmarkId c = *store.add("/d", std::nullopt, at_seconds(3), "", 0);
    CHECK(a.valid());
    CHECK(a != b);
    CHECK(store.list()[0].id == c);
    CHECK(store.list()[1].path == "/a/b");
    CHECK(store.contains("/a/b/"));
    CHECK(store.find(Path("/c"))->name == "Cee");
    CHECK(store.index_of(b) == 2);

    CHECK(store.add("/a/b", std::nullopt, {}).error().code == ErrorCode::AlreadyExists);
    CHECK(store.add("rel", std::nullopt, {}).error().code == ErrorCode::InvalidArgument);
    CHECK(store.add("", std::nullopt, {}).error().code == ErrorCode::InvalidArgument);
    CHECK(store.add("/x", std::nullopt, {}, "tab\there").error().code ==
          ErrorCode::InvalidArgument);
    CHECK(store.add("/x", std::nullopt, {}, std::string("\xff")).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(store.size() == 3);

    REQUIRE(store.rename(a, "Çalışma"));
    CHECK(store.find(a)->name == "Çalışma");
    CHECK(store.rename(a, std::string("\x7f")).error().code == ErrorCode::InvalidArgument);
    CHECK(store.rename(pl::BookmarkId{999}, "x").error().code == ErrorCode::NotFound);

    REQUIRE(store.move(c, 99)); // clamped to the end
    CHECK(store.list()[2].id == c);
    REQUIRE(store.move(c, 0));
    CHECK(store.list()[0].id == c);
    REQUIRE(store.move(b, 1));
    CHECK(store.list()[1].id == b);
    CHECK(store.move(pl::BookmarkId{999}, 0).error().code == ErrorCode::NotFound);

    const Result<pl::Bookmark> removed = store.remove(b);
    REQUIRE(removed);
    CHECK(removed->path == "/c");
    CHECK(store.remove(b).error().code == ErrorCode::NotFound);
    // Ids are never reused.
    const pl::BookmarkId d = *store.add("/c", std::nullopt, {});
    CHECK(d != b);
    CHECK(d.value > c.value);
}

TEST_CASE("places: BookmarkStore capacity", "[places][bookmarks]") {
    pl::BookmarkStore store;
    for (std::size_t i = 0; i < pl::kMaxBookmarks; ++i) {
        REQUIRE(store.add(Path("/b" + std::to_string(i)), std::nullopt, {}));
    }
    CHECK(store.add("/one-more", std::nullopt, {}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("places: Recents MRU order, counts and capacity", "[places][recents]") {
    pl::RecentsStore store(3);
    CHECK(store.capacity() == 3);
    CHECK(store.record("/a", std::nullopt, at_seconds(1)));
    CHECK(store.record("/b", std::nullopt, at_seconds(2)));
    CHECK(store.record("/c/", std::nullopt, at_seconds(3)));
    CHECK(store.record("/a", std::nullopt, at_seconds(4)));
    REQUIRE(store.size() == 3);
    CHECK(store.entries()[0].path == "/a");
    CHECK(store.entries()[0].visit_count == 2);
    CHECK(store.entries()[0].last_visit == at_seconds(4));
    CHECK(store.entries()[1].path == "/c");
    CHECK(store.entries()[2].path == "/b");
    CHECK(store.record("/d", std::nullopt, at_seconds(5)));
    REQUIRE(store.size() == 3);
    CHECK(store.find("/b") == nullptr); // least recently visited dropped
    CHECK(store.entries()[0].path == "/d");

    CHECK_FALSE(store.record("", std::nullopt, at_seconds(6)));
    CHECK_FALSE(store.record("rel", std::nullopt, at_seconds(6)));

    CHECK(store.remove("/c/"));
    CHECK_FALSE(store.remove("/c"));
    CHECK(store.size() == 2);

    CHECK(pl::RecentsStore(0).capacity() == 1);
    CHECK(pl::RecentsStore(100000).capacity() == pl::kMaxRecentsCapacity);
    CHECK(pl::RecentsStore().capacity() == pl::kDefaultRecentsCapacity);
}

TEST_CASE("places: Recents can be disabled and cleared", "[places][recents]") {
    pl::RecentsStore store;
    REQUIRE(store.record("/a", std::nullopt, at_seconds(1)));
    pl::RecentsOptions options;
    options.enabled = false;
    options.highlight = true;
    store.set_options(options);
    CHECK_FALSE(store.record("/b", std::nullopt, at_seconds(2)));
    CHECK_FALSE(store.record("/a", std::nullopt, at_seconds(2)));
    REQUIRE(store.size() == 1);
    CHECK(store.entries()[0].visit_count == 1);
    store.clear();
    CHECK(store.empty());
    CHECK(store.options() == options);
}

TEST_CASE("places: Recents keep offline entries and update the identity on revisit",
          "[places][recents]") {
    pl::RecentsStore store;
    const std::optional<VolumeIdentity> id = pl::identity_for("/media/u/STICK/x", system_volumes());
    REQUIRE(store.record("/media/u/STICK/x", id, at_seconds(1)));
    std::vector<VolumeInfo> gone = system_volumes();
    gone.pop_back();
    CHECK(store.resync(gone) == 0);
    REQUIRE(store.size() == 1);
    const pl::RecentEntry& e = store.entries()[0];
    CHECK(pl::resolve_location(e.path, e.volume, gone).status == pl::LocationStatus::Offline);
    REQUIRE(store.record("/media/u/STICK/x", std::nullopt, at_seconds(2)));
    CHECK_FALSE(store.entries()[0].volume.has_value());
}

TEST_CASE("places: Recents resync rebases and merges duplicates", "[places][recents]") {
    pl::RecentsStore store;
    const std::optional<VolumeIdentity> id = pl::identity_for("/media/u/STICK/x", system_volumes());
    REQUIRE(store.record("/media/u/STICK/x", id, at_seconds(10)));
    REQUIRE(store.record("/elsewhere", std::nullopt, at_seconds(11)));
    REQUIRE(store.record("/media/u/STICK/y", id, at_seconds(12)));
    // Visited at the new mount point already (newer), twice.
    std::vector<VolumeInfo> moved = system_volumes();
    moved.back().mount_point = "/media/u/STICK1";
    const std::optional<VolumeIdentity> moved_id = pl::identity_for("/media/u/STICK1/x", moved);
    REQUIRE(store.record("/media/u/STICK1/x", moved_id, at_seconds(20)));
    REQUIRE(store.record("/media/u/STICK1/x", moved_id, at_seconds(21)));
    // [STICK1/x (2, t21), STICK/y (t12), /elsewhere, STICK/x (t10)]
    const std::size_t changed = store.resync(moved);
    CHECK(changed == 3); // y rebased; x merged (survivor changed + one merged away)
    REQUIRE(store.size() == 3);
    CHECK(store.entries()[0].path == "/media/u/STICK1/x");
    CHECK(store.entries()[0].visit_count == 3);
    CHECK(store.entries()[0].last_visit == at_seconds(21));
    CHECK(store.entries()[1].path == "/media/u/STICK1/y");
    CHECK(store.entries()[2].path == "/elsewhere");
    CHECK(store.resync(moved) == 0);

    // Merge keeps the newer entry's position when the rebased one is newer.
    pl::RecentsStore other;
    REQUIRE(other.record("/media/u/STICK1/z", std::nullopt, at_seconds(1)));
    REQUIRE(other.record("/media/u/STICK/z", id, at_seconds(5)));
    CHECK(other.resync(moved) == 2);
    REQUIRE(other.size() == 1);
    CHECK(other.entries()[0].last_visit == at_seconds(5));
    CHECK(other.entries()[0].visit_count == 2);
    REQUIRE(other.entries()[0].volume.has_value());
    CHECK(must(other.entries()[0].volume).mount_point == "/media/u/STICK1");
}

TEST_CASE("places: Recents visit counts saturate", "[places][recents]") {
    // Built through JSON (the only way to set a count) to avoid 4e9 record() calls.
    const Result<pl::RecentsStore> store =
        pl::recents_from_json(R"({"recents": [{"path": "/a", "visits": 4294967295}]})");
    REQUIRE(store);
    pl::RecentsStore s = *store;
    REQUIRE(s.record("/a", std::nullopt, at_seconds(1)));
    CHECK(s.entries()[0].visit_count == std::numeric_limits<std::uint32_t>::max());
}
