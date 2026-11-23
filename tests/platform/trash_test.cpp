// ITrash (trash.hpp, rules T1..T7) on fixture directories only: a fake home trash under a
// temporary directory, fake mount tables that list only fixture mount points, fake topdirs, and
// a /dev/shm fixture for the second filesystem. Every trash root and item is checked with
// require_in_fixture() (trash_fixture.hpp) before and after each operation.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <navigator/platform.hpp>
#include <navigator/trash.hpp>

#include "trash_fixture.hpp"
#include "trash_internal.hpp"

namespace {

namespace stdfs = std::filesystem;
using navigator::ErrorCode;
using navigator::Path;
using navigator::platform_test::device_of;
using navigator::platform_test::exists_nofollow;
using navigator::platform_test::fixture_options;
using navigator::platform_test::FixtureDir;
using navigator::platform_test::kFixedDate;
using navigator::platform_test::make_fixture_trash;
using navigator::platform_test::mode_bits;
using navigator::platform_test::mount_line;
using navigator::platform_test::my_uid;
using navigator::platform_test::OtherFsFixture;
using navigator::platform_test::read_text;
using navigator::platform_test::require_in_fixture;
using navigator::platform_test::ScopedEnv;
using navigator::platform_test::write_file;
using navigator::trash::escape_path;
using navigator::trash::TrashDir;
using navigator::trash::TrashDirKind;
using navigator::trash::TrashedItem;
namespace ti = navigator::trash_internal;
namespace fs = navigator::fs;

std::size_t count_entries(const Path& dir) {
    std::error_code ec;
    if (!stdfs::exists(dir, ec)) {
        return 0;
    }
    return static_cast<std::size_t>(
        std::distance(stdfs::directory_iterator(dir), stdfs::directory_iterator()));
}

std::string expected_info(const Path& original_value) {
    return "[Trash Info]\nPath=" + escape_path(original_value.native()) +
           "\nDeletionDate=2026-11-23T14:05:09\n";
}

std::string date_text(const std::tm& tm) {
    return navigator::trash::format_deletion_date(navigator::trash::LocalDateTime{
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec});
}

} // namespace

TEST_CASE("trash create_trash options", "[trash]") {
    FixtureDir fx;
    SECTION("relative home trash") {
        navigator::trash::TrashOptions o;
        o.home_trash = "rel/Trash";
        CHECK(navigator::trash::create_trash(o).error().code == ErrorCode::InvalidArgument);
    }
    SECTION("default home trash is $XDG_DATA_HOME/Trash") {
        ScopedEnv data("XDG_DATA_HOME", (fx / "xdg").native());
        navigator::trash::TrashOptions o;
        o.use_topdir_trashes = false;
        auto t = navigator::trash::create_trash(o);
        REQUIRE(t.has_value());
        auto dirs = (*t)->trash_dirs();
        REQUIRE(dirs.has_value());
        REQUIRE(dirs->size() == 1);
        CHECK((*dirs)[0].root == fx / "xdg" / "Trash");
        CHECK((*dirs)[0].kind == TrashDirKind::Home);
        CHECK((*dirs)[0].topdir.empty());
        CHECK_FALSE(exists_nofollow(fx / "xdg")); // never creates anything
    }
}

TEST_CASE("trash home trash: files, info, modes, exact .trashinfo", "[trash]") {
    FixtureDir fx;
    auto trash = make_fixture_trash(fixture_options(fx.path()));
    const Path home = fx / "data" / "Trash";
    const Path src = fx / "Masaüstü" / "rapor son%.pdf";
    write_file(src, "hello");
    require_in_fixture(src);

    auto item = trash->trash(src);
    REQUIRE(item.has_value());
    require_in_fixture(*item);
    CHECK_FALSE(exists_nofollow(src));
    CHECK(item->name == "rapor son%.pdf");
    CHECK(item->dir.kind == TrashDirKind::Home);
    CHECK(item->dir.root == home);
    CHECK(item->dir.topdir.empty());
    CHECK(item->dir.device == device_of(home));
    CHECK(item->original_path == src);
    CHECK(item->deletion_date == kFixedDate);
    CHECK(item->kind == fs::EntryKind::File);
    CHECK(item->size == 5);
    CHECK(item->id.inode != 0);
    CHECK(read_text(item->files_path()) == "hello");
    CHECK(read_text(item->info_path()) == expected_info(src));
    CHECK(read_text(item->info_path()).find("%C3%BC") != std::string::npos); // "ü" escaped
    CHECK(mode_bits(home) == 0700);
    CHECK(mode_bits(home / "files") == 0700);
    CHECK(mode_bits(home / "info") == 0700);
    CHECK(mode_bits(item->info_path()) == 0600);
    CHECK(mode_bits(fx / "data") == 0700);                 // missing parents created 0700 like GIO
    CHECK_FALSE(exists_nofollow(home / "directorysizes")); // T6

    SECTION("a symlink is trashed as a link, never followed (T2)") {
        const Path target = fx / "target.txt";
        write_file(target, "keep");
        const Path link = fx / "link";
        stdfs::create_symlink(target, link);
        auto li = trash->trash(link);
        REQUIRE(li.has_value());
        CHECK(li->kind == fs::EntryKind::Symlink);
        CHECK(stdfs::is_symlink(li->files_path()));
        CHECK(read_text(target) == "keep");
        const Path dangling = fx / "dangling";
        stdfs::create_symlink(fx / "nowhere", dangling);
        auto di = trash->trash(dangling);
        REQUIRE(di.has_value());
        CHECK(di->kind == fs::EntryKind::Symlink);
    }
    SECTION("a directory tree moves as one rename") {
        write_file(fx / "dir" / "sub" / "f", "x");
        auto d = trash->trash(fx / "dir");
        REQUIRE(d.has_value());
        CHECK(d->kind == fs::EntryKind::Directory);
        CHECK(d->size == 0);
        CHECK(read_text(d->files_path() / "sub" / "f") == "x");
    }
    SECTION("a trailing slash and dot segments are normalized") {
        write_file(fx / "n" / "x", "1");
        auto d = trash->trash(fx.path() / "n/./" / "");
        REQUIRE(d.has_value());
        CHECK(d->original_path == fx / "n");
    }
}

TEST_CASE("trash DeletionDate is local time from the system clock", "[trash]") {
    FixtureDir fx;
    {
        ScopedEnv tz("TZ", std::string("UTC-14")); // 14 hours east: local != UTC on any day
        ::tzset();
        auto o = fixture_options(fx.path());
        o.now = nullptr;
        auto trash = make_fixture_trash(o);
        write_file(fx / "f", "x");
        const std::time_t before = std::time(nullptr);
        auto item = trash->trash(fx / "f");
        const std::time_t after = std::time(nullptr);
        REQUIRE(item.has_value());
        std::tm lb{};
        std::tm la{};
        std::tm ub{};
        ::localtime_r(&before, &lb);
        ::localtime_r(&after, &la);
        ::gmtime_r(&before, &ub);
        const std::string written = read_text(item->info_path());
        const std::string date = written.substr(written.find("DeletionDate=") + 13, 19);
        CHECK((date == date_text(lb) || date == date_text(la)));
        CHECK(date != date_text(ub));
        CHECK(item->deletion_date == navigator::trash::parse_deletion_date(date));
    }
    ::tzset(); // the previous TZ is back
}

TEST_CASE("trash unique names under collisions (T5)", "[trash]") {
    FixtureDir fx;
    auto trash = make_fixture_trash(fixture_options(fx.path()));
    const Path home = fx / "data" / "Trash";
    std::vector<std::string> names;
    for (int i = 0; i < 3; ++i) {
        const Path p = fx / ("d" + std::to_string(i)) / "x.txt";
        write_file(p, std::to_string(i));
        auto item = trash->trash(p);
        REQUIRE(item.has_value());
        names.push_back(item->name);
        CHECK(read_text(item->info_path()) == expected_info(p));
    }
    CHECK(names == std::vector<std::string>{"x.txt", "x.txt.2", "x.txt.3"});

    // files/x.txt.4 without an info file (an orphan) and a stray info/x.txt.5.trashinfo: both
    // names are skipped and the reserved info file of the failed candidate is removed again.
    write_file(home / "files" / "x.txt.4", "orphan");
    write_file(home / "info" / "x.txt.5.trashinfo", "stray");
    const Path p = fx / "d9" / "x.txt";
    write_file(p, "9");
    auto item = trash->trash(p);
    REQUIRE(item.has_value());
    CHECK(item->name == "x.txt.6");
    CHECK_FALSE(exists_nofollow(home / "info" / "x.txt.4.trashinfo"));
    CHECK(read_text(home / "files" / "x.txt.4") == "orphan");
    CHECK(read_text(home / "info" / "x.txt.5.trashinfo") == "stray");

    SECTION("a 255-byte Turkish name is cut at a code point boundary") {
        std::string longname;
        for (int i = 0; i < 127; ++i) {
            longname += "ş";
        }
        longname += "x"; // 255 bytes
        const Path lp = fx / "long" / longname;
        write_file(lp, "L");
        auto li = trash->trash(lp);
        REQUIRE(li.has_value());
        CHECK(li->name.size() == 244);
        CHECK(li->name == longname.substr(0, 244));
        CHECK(li->original_path == lp); // Path= keeps the whole name
        CHECK(read_text(li->info_path()) == expected_info(lp));
        // Second one with the same long name: suffix still fits.
        write_file(fx / "long2" / longname, "M");
        auto l2 = trash->trash(fx / "long2" / longname);
        REQUIRE(l2.has_value());
        CHECK(l2->name.ends_with(".2"));
        CHECK(l2->name.size() + 10 <= 255);
    }
}

TEST_CASE("trash concurrent trash of the same name from 4 threads", "[trash][threads]") {
    FixtureDir fx;
    auto trash = make_fixture_trash(fixture_options(fx.path()));
    constexpr int kThreads = 4;
    constexpr int kRounds = 8;
    std::vector<Path> sources;
    for (int t = 0; t < kThreads; ++t) {
        for (int r = 0; r < kRounds; ++r) {
            const Path p = fx / ("t" + std::to_string(t) + "r" + std::to_string(r)) / "same";
            write_file(p, std::to_string(t * 100 + r));
            sources.push_back(p);
        }
    }
    std::vector<std::vector<TrashedItem>> results(kThreads);
    std::atomic<int> failures{0};
    std::atomic<int> ready{0};
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                ready.fetch_add(1);
                while (ready.load() < kThreads) {
                    std::this_thread::yield();
                }
                for (int r = 0; r < kRounds; ++r) {
                    auto item = trash->trash(sources[(static_cast<std::size_t>(t) * kRounds) +
                                                     static_cast<std::size_t>(r)]);
                    if (item) {
                        results[static_cast<std::size_t>(t)].push_back(std::move(*item));
                    } else {
                        failures.fetch_add(1);
                    }
                }
            });
        }
    }
    CHECK(failures.load() == 0);
    std::set<std::string> names;
    for (const auto& per_thread : results) {
        for (const TrashedItem& item : per_thread) {
            require_in_fixture(item);
            CHECK(names.insert(item.name).second);
            CHECK(read_text(item.info_path()) == expected_info(item.original_path));
        }
    }
    CHECK(names.size() == static_cast<std::size_t>(kThreads * kRounds));
    CHECK(names.count("same") == 1);
    CHECK(names.count("same.32") == 1);
    CHECK(count_entries(fx / "data" / "Trash" / "files") == names.size());
    CHECK(count_entries(fx / "data" / "Trash" / "info") == names.size());
    for (const Path& p : sources) {
        CHECK_FALSE(exists_nofollow(p));
    }
}

TEST_CASE("trash refuses protected sources and changes nothing (T7)", "[trash]") {
    FixtureDir fx;
    auto trash = make_fixture_trash(fixture_options(fx.path()));
    const Path home = fx / "data" / "Trash";
    write_file(fx / "keep", "k");
    // A home trash with one item, so "inside the trash" paths exist.
    write_file(fx / "t1", "t");
    auto t1 = trash->trash(fx / "t1");
    REQUIRE(t1.has_value());
    const std::string uid = std::to_string(my_uid());
    write_file(fx / "vol" / (".Trash-" + uid) / "files" / "x", "x");
    write_file(fx / "vol" / ".Trash" / uid / "files" / "y", "y");

    for (const Path& bad :
         {Path(""), Path("rel/x"), Path("/"), home, home / "files", t1->files_path(),
          t1->info_path(), fx / "data", fx / "vol" / (".Trash-" + uid),
          fx / "vol" / (".Trash-" + uid) / "files" / "x",
          fx / "vol" / ".Trash" / uid / "files" / "y"}) {
        INFO(bad.native());
        auto r = trash->trash(bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::InvalidArgument);
        auto d = trash->trash_dir_for(bad);
        REQUIRE_FALSE(d.has_value());
        CHECK(d.error().code == ErrorCode::InvalidArgument);
    }
    CHECK(count_entries(home / "files") == 1);
    CHECK(count_entries(home / "info") == 1);
    CHECK(exists_nofollow(fx / "vol" / (".Trash-" + uid) / "files" / "x"));
    // The ancestor check is lexical on the canonical path as well: through a symlink.
    stdfs::create_directory_symlink(fx / "data", fx / "datalink");
    CHECK(trash->trash(fx / "datalink" / "Trash").error().code == ErrorCode::InvalidArgument);

    SECTION("missing item: NotFound, and the home trash is not created") {
        FixtureDir fresh;
        auto t = make_fixture_trash(fixture_options(fresh.path()));
        CHECK(t->trash(fresh / "missing").error().code == ErrorCode::NotFound);
        CHECK(t->trash(fresh / "nodir" / "missing").error().code == ErrorCode::NotFound);
        write_file(fresh / "file", "f");
        CHECK(t->trash(fresh / "file" / "below").error().code == ErrorCode::NotFound);
        CHECK_FALSE(exists_nofollow(fresh / "data"));
    }
    SECTION("unwritable parent: PermissionDenied, nothing reserved") {
        if (::geteuid() != 0) {
            write_file(fx / "ro" / "f", "f");
            stdfs::permissions(fx / "ro", stdfs::perms::owner_read | stdfs::perms::owner_exec);
            auto r = trash->trash(fx / "ro" / "f");
            stdfs::permissions(fx / "ro", stdfs::perms::owner_all);
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error().code == ErrorCode::PermissionDenied);
            CHECK(count_entries(home / "info") == 1);
            CHECK(exists_nofollow(fx / "ro" / "f"));
        }
    }
}

TEST_CASE("trash is_inside_trash is lexical", "[trash]") {
    FixtureDir fx;
    auto o = fixture_options(fx.path());
    o.uid = 1234;
    auto trash = make_fixture_trash(o);
    const Path home = fx / "data" / "Trash";
    CHECK(trash->is_inside_trash(home));
    CHECK(trash->is_inside_trash(home / "files" / "a"));
    CHECK(trash->is_inside_trash(fx.path() / "data/Trash/"));
    CHECK(trash->is_inside_trash(fx.path() / "data/x/../Trash/info"));
    CHECK_FALSE(trash->is_inside_trash(fx / "data"));
    CHECK_FALSE(trash->is_inside_trash(fx / "data" / "Trash2"));
    CHECK(trash->is_inside_trash("/media/u/S/.Trash-1234"));
    CHECK(trash->is_inside_trash("/media/u/S/.Trash-1234/files/x"));
    CHECK(trash->is_inside_trash("/media/u/S/.Trash/1234"));
    CHECK(trash->is_inside_trash("/media/u/S/.Trash/1234/info/x.trashinfo"));
    CHECK_FALSE(trash->is_inside_trash("/media/u/S/.Trash"));
    CHECK_FALSE(trash->is_inside_trash("/media/u/S/.Trash/999"));
    CHECK_FALSE(trash->is_inside_trash("/media/u/S/.Trash-999"));
    CHECK_FALSE(trash->is_inside_trash("/media/u/S/1234"));
    CHECK_FALSE(trash->is_inside_trash(""));
}

TEST_CASE("trash restore: identity, conflicts, keep both, missing parent", "[trash]") {
    FixtureDir fx;
    auto trash = make_fixture_trash(fixture_options(fx.path()));
    const Path src = fx / "docs" / "a.txt";
    write_file(src, "original");
    auto item = trash->trash(src);
    REQUIRE(item.has_value());
    require_in_fixture(*item);

    SECTION("back to the original path; info removed") {
        REQUIRE(trash->restore(*item, item->original_path).has_value());
        CHECK(read_text(src) == "original");
        CHECK_FALSE(exists_nofollow(item->files_path()));
        CHECK_FALSE(exists_nofollow(item->info_path()));
        // A second restore of the same listing entry: gone.
        CHECK(trash->restore(*item, src).error().code == ErrorCode::NotFound);
    }
    SECTION("conflict: AlreadyExists, never overwrites, item stays") {
        write_file(src, "newer");
        CHECK(trash->restore(*item, src).error().code == ErrorCode::AlreadyExists);
        CHECK(read_text(src) == "newer");
        CHECK(read_text(item->files_path()) == "original");
        CHECK(exists_nofollow(item->info_path()));
        // Keep both: another name in the same folder.
        REQUIRE(trash->restore(*item, fx / "docs" / "a (2).txt").has_value());
        CHECK(read_text(fx / "docs" / "a (2).txt") == "original");
        CHECK(read_text(src) == "newer");
    }
    SECTION("conflict with a directory destination") {
        stdfs::create_directories(src);
        CHECK(trash->restore(*item, src).error().code == ErrorCode::AlreadyExists);
    }
    SECTION("identity mismatch: a different item under the same trash name") {
        // The impostor exists before the original goes, so it cannot reuse the inode.
        write_file(fx / "impostor", "impostor");
        stdfs::rename(fx / "impostor", item->files_path()); // replaces the trashed file
        CHECK(trash->restore(*item, src).error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(exists_nofollow(src));
        CHECK(read_text(item->files_path()) == "impostor");
        TrashedItem unknown = *item;
        unknown.id = {};
        CHECK(trash->restore(unknown, src).error().code == ErrorCode::InvalidArgument);
    }
    SECTION("missing parent folder: NotFound (the ops engine creates it)") {
        stdfs::remove_all(fx / "docs");
        CHECK(trash->restore(*item, src).error().code == ErrorCode::NotFound);
        write_file(fx / "docs", "a file, not a folder");
        CHECK(trash->restore(*item, src).error().code == ErrorCode::NotFound);
        CHECK(exists_nofollow(item->files_path()));
    }
    SECTION("relative destination") {
        CHECK(trash->restore(*item, "docs/a.txt").error().code == ErrorCode::InvalidArgument);
        CHECK(trash->restore(*item, "").error().code == ErrorCode::InvalidArgument);
    }
    SECTION("another filesystem: CrossDevice") {
        OtherFsFixture shm("/dev/shm", fx.path());
        if (!shm.usable()) {
            SKIP("/dev/shm is not a separate writable filesystem here");
        }
        CHECK(trash->restore(*item, shm / "a.txt").error().code == ErrorCode::CrossDevice);
        CHECK(exists_nofollow(item->files_path()));
        CHECK(exists_nofollow(item->info_path()));
    }
}

TEST_CASE("trash forget ordering", "[trash]") {
    FixtureDir fx;
    auto trash = make_fixture_trash(fixture_options(fx.path()));
    write_file(fx / "f", "x");
    auto item = trash->trash(fx / "f");
    REQUIRE(item.has_value());
    // files/<name> still there: forget would orphan data.
    CHECK(trash->forget(*item).error().code == ErrorCode::InvalidArgument);
    CHECK(exists_nofollow(item->info_path()));
    // The ops engine erased the data (here: the test, inside the fixture).
    require_in_fixture(item->files_path());
    stdfs::remove(item->files_path());
    REQUIRE(trash->forget(*item).has_value());
    CHECK_FALSE(exists_nofollow(item->info_path()));
    CHECK(trash->forget(*item).error().code == ErrorCode::NotFound);
}

TEST_CASE("trash list tolerates corrupt and orphan entries", "[trash]") {
    FixtureDir fx;
    auto trash = make_fixture_trash(fixture_options(fx.path()));
    const Path home = fx / "data" / "Trash";

    SECTION("no home trash yet: empty listing, no error") {
        auto l = trash->list(std::stop_token{});
        REQUIRE(l.has_value());
        CHECK(l->items.empty());
        CHECK(l->orphans == 0);
        CHECK(l->dir_errors.empty());
    }

    // What GIO / Nautilus leave behind (escaped Turkish path), what KDE writes (raw), one
    // without a date, and broken entries.
    write_file(home / "files" / "fotoğraf 1.jpg", "jpeg");
    write_file(home / "info" / "fotoğraf 1.jpg.trashinfo",
               "[Trash Info]\nPath=/home/o%C4%9Fuz/Resimler/foto%C4%9Fraf%201.jpg\n"
               "DeletionDate=2026-11-20T08:00:59\n");
    stdfs::create_directories(home / "files" / "klasör");
    write_file(home / "info" / "klasör.trashinfo",
               "[Trash Info]\r\nPath=/home/oğuz/klasör\r\nDeletionDate=2026-11-21T10:00:00");
    write_file(home / "files" / "nodate", "n");
    write_file(home / "info" / "nodate.trashinfo", "[Trash Info]\nPath=/home/u/nodate\n");
    // Orphans and garbage.
    write_file(home / "info" / "gone.trashinfo", "[Trash Info]\nPath=/home/u/gone\n");
    write_file(home / "files" / "noinfo", "x");
    write_file(home / "files" / "corrupt", "c");
    write_file(home / "info" / "corrupt.trashinfo", "\x01\x02 not an info file");
    write_file(home / "files" / "relative", "r");
    write_file(home / "info" / "relative.trashinfo", "[Trash Info]\nPath=relative/in/home\n");
    write_file(home / "info" / "README", "not a .trashinfo name: ignored");
    write_file(home / "info" / ".trashinfo", "[Trash Info]\nPath=/x\n");
    write_file(home / "directorysizes", "ignored (T6)");
    stdfs::create_directories(home / "info" / "dir.trashinfo"); // a directory, not a file
    write_file(home / "info" / "big.trashinfo",
               "[Trash Info]\nPath=/big\n" + std::string(std::size_t{70} * 1024, '#'));
    write_file(home / "files" / "big", "b");

    auto listing = trash->list(std::stop_token{});
    REQUIRE(listing.has_value());
    REQUIRE(listing->items.size() == 3);
    for (const TrashedItem& item : listing->items) {
        require_in_fixture(item.dir);
        require_in_fixture(item.files_path());
    }
    // Newest first; undated last.
    CHECK(listing->items[0].name == "klasör");
    CHECK(listing->items[0].original_path == Path("/home/oğuz/klasör"));
    CHECK(listing->items[0].kind == fs::EntryKind::Directory);
    CHECK(listing->items[1].name == "fotoğraf 1.jpg");
    CHECK(listing->items[1].original_path == Path("/home/oğuz/Resimler/fotoğraf 1.jpg"));
    CHECK(listing->items[1].deletion_date ==
          navigator::trash::LocalDateTime{2026, 11, 20, 8, 0, 59});
    CHECK(listing->items[1].size == 4);
    CHECK(listing->items[1].kind == fs::EntryKind::File);
    CHECK(listing->items[2].name == "nodate");
    CHECK_FALSE(listing->items[2].deletion_date.has_value());
    // gone (info only), noinfo (file only), corrupt, relative-in-home, dir.trashinfo, big.
    CHECK(listing->orphans == 6);
    CHECK(listing->dir_errors.empty());
    CHECK(listing->items[1].dir == listing->items[0].dir);
    CHECK(listing->items[0].dir.kind == TrashDirKind::Home);

    SECTION("a listed item restores and the identity matches") {
        const Path dest = fx / "restored.jpg";
        REQUIRE(trash->restore(listing->items[1], dest).has_value());
        CHECK(read_text(dest) == "jpeg");
    }
    SECTION("cancellation") {
        std::stop_source stop;
        stop.request_stop();
        CHECK(trash->list(stop.get_token()).error().code == ErrorCode::Cancelled);
    }
    SECTION("an unreadable info dir is a dir error, not a failure") {
        if (::geteuid() != 0) {
            stdfs::permissions(home / "info", stdfs::perms::none);
            auto l = trash->list(std::stop_token{});
            stdfs::permissions(home / "info", stdfs::perms::owner_all);
            REQUIRE(l.has_value());
            CHECK(l->items.empty());
            CHECK(l->dir_errors.size() == 1);
        }
    }
}

TEST_CASE("trash topdir selection matrix (T4)", "[trash][topdir]") {
    FixtureDir fx;
    const Path top = fx / "vol";
    stdfs::create_directories(top);
    require_in_fixture(top);
    const std::uint32_t uid = my_uid();
    const std::string u = std::to_string(uid);

    SECTION("nothing exists: .Trash-$uid, created 0700 only when asked") {
        auto probe = ti::select_topdir_trash(top, uid, false);
        REQUIRE(probe.has_value());
        CHECK(probe->kind == TrashDirKind::User);
        CHECK(probe->root == top / (".Trash-" + u));
        CHECK(probe->topdir == top);
        CHECK_FALSE(exists_nofollow(top / (".Trash-" + u)));
        auto made = ti::select_topdir_trash(top, uid, true);
        REQUIRE(made.has_value());
        CHECK(*made == *probe);
        CHECK(mode_bits(top / (".Trash-" + u)) == 0700);
    }
    SECTION(".Trash with the sticky bit: .Trash/$uid, created 0700") {
        stdfs::create_directories(top / ".Trash");
        ::chmod((top / ".Trash").c_str(), 01777);
        auto probe = ti::select_topdir_trash(top, uid, false);
        REQUIRE(probe.has_value());
        CHECK(probe->kind == TrashDirKind::SharedUser);
        CHECK_FALSE(exists_nofollow(top / ".Trash" / u));
        auto made = ti::select_topdir_trash(top, uid, true);
        REQUIRE(made.has_value());
        CHECK(made->kind == TrashDirKind::SharedUser);
        CHECK(made->root == top / ".Trash" / u);
        CHECK(mode_bits(top / ".Trash" / u) == 0700);
        CHECK(mode_bits(top / ".Trash") == 01777); // never "fixed"
        CHECK_FALSE(exists_nofollow(top / (".Trash-" + u)));
    }
    SECTION(".Trash without the sticky bit is ignored") {
        stdfs::create_directories(top / ".Trash");
        ::chmod((top / ".Trash").c_str(), 0777);
        auto made = ti::select_topdir_trash(top, uid, true);
        REQUIRE(made.has_value());
        CHECK(made->kind == TrashDirKind::User);
        CHECK_FALSE(exists_nofollow(top / ".Trash" / u));
        CHECK(mode_bits(top / ".Trash") == 0777);
    }
    SECTION("a symlinked .Trash is refused even when its target is sticky") {
        stdfs::create_directories(fx / "elsewhere");
        ::chmod((fx / "elsewhere").c_str(), 01777);
        stdfs::create_directory_symlink(fx / "elsewhere", top / ".Trash");
        auto made = ti::select_topdir_trash(top, uid, true);
        REQUIRE(made.has_value());
        CHECK(made->kind == TrashDirKind::User);
        CHECK(count_entries(fx / "elsewhere") == 0);
    }
    SECTION(".Trash is a file") {
        write_file(top / ".Trash", "file");
        auto made = ti::select_topdir_trash(top, uid, true);
        REQUIRE(made.has_value());
        CHECK(made->kind == TrashDirKind::User);
    }
    SECTION("sticky .Trash but .Trash/$uid is a symlink or foreign: method (2)") {
        stdfs::create_directories(top / ".Trash");
        ::chmod((top / ".Trash").c_str(), 01777);
        stdfs::create_directories(fx / "mine");
        stdfs::create_directory_symlink(fx / "mine", top / ".Trash" / u);
        auto made = ti::select_topdir_trash(top, uid, true);
        REQUIRE(made.has_value());
        CHECK(made->kind == TrashDirKind::User);
        auto other = ti::select_topdir_trash(top, uid + 1, true); // .Trash/<uid+1> owned by us
        REQUIRE_FALSE(other.has_value());                         // and .Trash-<uid+1> too
        CHECK(other.error().code == ErrorCode::Unsupported);
    }
    SECTION("existing .Trash-$uid: symlink, file or foreign owner -> Unsupported") {
        stdfs::create_directories(fx / "target");
        stdfs::create_directory_symlink(fx / "target", top / (".Trash-" + u));
        CHECK(ti::select_topdir_trash(top, uid, true).error().code == ErrorCode::Unsupported);
        CHECK(ti::select_topdir_trash(top, uid, false).error().code == ErrorCode::Unsupported);
        stdfs::remove(top / (".Trash-" + u));
        write_file(top / (".Trash-" + u), "file");
        CHECK(ti::select_topdir_trash(top, uid, true).error().code == ErrorCode::Unsupported);
        stdfs::remove(top / (".Trash-" + u));
        const std::string foreign = std::to_string(uid + 1);
        stdfs::create_directories(top / (".Trash-" + foreign)); // owned by us, not by uid+1
        CHECK(ti::select_topdir_trash(top, uid + 1, true).error().code == ErrorCode::Unsupported);
        CHECK(count_entries(fx / "target") == 0);
    }
    SECTION("unwritable topdir: Unsupported, nothing created") {
        if (::geteuid() != 0) {
            ::chmod(top.c_str(), 0555);
            auto probe = ti::select_topdir_trash(top, uid, false);
            auto made = ti::select_topdir_trash(top, uid, true);
            ::chmod(top.c_str(), 0755);
            CHECK(probe.error().code == ErrorCode::Unsupported);
            CHECK(made.error().code == ErrorCode::Unsupported);
            CHECK(count_entries(top) == 0);
        }
    }
    SECTION("existing_topdir_trashes lists only valid ones, never creates") {
        CHECK(ti::existing_topdir_trashes(top, uid).empty());
        stdfs::create_directories(top / ".Trash" / u);
        ::chmod((top / ".Trash").c_str(), 01777);
        stdfs::create_directories(top / (".Trash-" + u));
        auto both = ti::existing_topdir_trashes(top, uid);
        REQUIRE(both.size() == 2);
        CHECK(both[0].kind == TrashDirKind::SharedUser);
        CHECK(both[1].kind == TrashDirKind::User);
        ::chmod((top / ".Trash").c_str(), 0777); // sticky bit gone: (1) no longer valid
        auto one = ti::existing_topdir_trashes(top, uid);
        REQUIRE(one.size() == 1);
        CHECK(one[0].kind == TrashDirKind::User);
        CHECK(count_entries(top) == 2);
    }
}

TEST_CASE("trash trash_dirs from a fixture mount table", "[trash][topdir]") {
    FixtureDir fx;
    const std::string u = std::to_string(my_uid());
    const Path a = fx / "mnt" / "a"; // .Trash-$uid
    const Path b = fx / "mnt" / "b"; // sticky .Trash/$uid
    const Path c = fx / "mnt" / "c"; // nothing
    const Path d = fx / "mnt" / "d"; // symlinked .Trash-$uid (invalid)
    stdfs::create_directories(a / (".Trash-" + u));
    stdfs::create_directories(b / ".Trash" / u);
    ::chmod((b / ".Trash").c_str(), 01777);
    stdfs::create_directories(c);
    stdfs::create_directories(d);
    stdfs::create_directory_symlink(a / (".Trash-" + u), d / (".Trash-" + u));
    const std::string table =
        mount_line(30, a) + mount_line(31, b) + mount_line(32, c) + mount_line(33, d, "vfat");
    auto trash = make_fixture_trash(fixture_options(fx.path(), table));
    auto dirs = trash->trash_dirs();
    REQUIRE(dirs.has_value());
    REQUIRE(dirs->size() == 3);
    for (const TrashDir& dir : *dirs) {
        require_in_fixture(dir);
    }
    CHECK((*dirs)[0].kind == TrashDirKind::Home);
    CHECK((*dirs)[0].root == fx / "data" / "Trash");
    CHECK((*dirs)[1] == TrashDir{TrashDirKind::User, a / (".Trash-" + u), a, device_of(a)});
    CHECK((*dirs)[2] == TrashDir{TrashDirKind::SharedUser, b / ".Trash" / u, b, device_of(b)});
    CHECK_FALSE(exists_nofollow(fx / "data"));
    CHECK(count_entries(c) == 0);

    // Listing reads every trash directory; relative Path= values resolve against the topdir.
    write_file(a / (".Trash-" + u) / "files" / "x", "x");
    write_file(a / (".Trash-" + u) / "info" / "x.trashinfo",
               "[Trash Info]\nPath=Belgeler/x\nDeletionDate=2026-11-23T10:00:00\n");
    write_file(b / ".Trash" / u / "files" / "y", "y");
    write_file(b / ".Trash" / u / "info" / "y.trashinfo",
               "[Trash Info]\nPath=../../escape\nDeletionDate=2026-11-23T10:00:00\n");
    auto listing = trash->list(std::stop_token{});
    REQUIRE(listing.has_value());
    REQUIRE(listing->items.size() == 1);
    CHECK(listing->items[0].original_path == a / "Belgeler" / "x");
    CHECK(listing->items[0].dir.kind == TrashDirKind::User);
    CHECK(listing->orphans == 1); // y: Path escapes its topdir

    SECTION("use_topdir_trashes = false: home only") {
        auto o = fixture_options(fx.path(), table);
        o.use_topdir_trashes = false;
        auto home_only = make_fixture_trash(o);
        CHECK(home_only->trash_dirs().value().size() == 1);
    }
    SECTION("a failing mount table fails trash_dirs") {
        auto o = fixture_options(fx.path(), table);
        auto src = std::make_shared<navigator::platform_test::FakeMountSource>(table);
        src->fail_next = true;
        o.volumes.source = src;
        auto t = make_fixture_trash(o);
        CHECK(t->trash_dirs().error().code == ErrorCode::IoError);
    }
}

TEST_CASE("trash per-volume trash on a second filesystem (T1, T4)", "[trash][topdir]") {
    FixtureDir fx; // home trash lives here
    OtherFsFixture shm("/dev/shm", fx.path());
    if (!shm.usable()) {
        SKIP("/dev/shm is not a separate writable filesystem here");
    }
    const Path vol = shm / "vol";
    const std::string u = std::to_string(my_uid());
    write_file(vol / "Belgeler" / "rapor ğ.txt", "data");
    const Path item_path = vol / "Belgeler" / "rapor ğ.txt";
    require_in_fixture(item_path);
    const std::string table = mount_line(40, vol, "ext4");
    auto trash = make_fixture_trash(fixture_options(fx.path(), table));

    SECTION("trash_dir_for probes without creating anything") {
        auto d = trash->trash_dir_for(item_path);
        REQUIRE(d.has_value());
        require_in_fixture(*d);
        CHECK(d->kind == TrashDirKind::User);
        CHECK(d->root == vol / (".Trash-" + u));
        CHECK_FALSE(exists_nofollow(vol / (".Trash-" + u)));
        auto h = trash->trash_dir_for(fx / "nothing-here");
        CHECK(h.error().code == ErrorCode::NotFound);
        write_file(fx / "home-item", "h");
        auto hd = trash->trash_dir_for(fx / "home-item");
        REQUIRE(hd.has_value());
        CHECK(hd->kind == TrashDirKind::Home);
        CHECK_FALSE(exists_nofollow(fx / "data"));
    }
    SECTION("trash, list and restore with a relative Path=") {
        auto item = trash->trash(item_path);
        REQUIRE(item.has_value());
        require_in_fixture(*item);
        CHECK(item->dir.kind == TrashDirKind::User);
        CHECK(item->dir.root == vol / (".Trash-" + u));
        CHECK(item->dir.topdir == vol);
        CHECK(item->dir.device == device_of(vol));
        CHECK(item->original_path == item_path);
        CHECK(read_text(item->info_path()) ==
              "[Trash Info]\nPath=Belgeler/rapor%20%C4%9F.txt\nDeletionDate=2026-11-23T14:05:09\n");
        CHECK(mode_bits(vol / (".Trash-" + u)) == 0700);
        CHECK(mode_bits(vol / (".Trash-" + u) / "files") == 0700);
        CHECK(mode_bits(vol / (".Trash-" + u) / "info") == 0700);
        CHECK_FALSE(exists_nofollow(fx / "data")); // the home trash was not touched

        auto listing = trash->list(std::stop_token{});
        REQUIRE(listing.has_value());
        REQUIRE(listing->items.size() == 1);
        CHECK(listing->items[0] == *item);

        REQUIRE(trash->restore(listing->items[0], item_path).has_value());
        CHECK(read_text(item_path) == "data");
        CHECK(count_entries(vol / (".Trash-" + u) / "info") == 0);
    }
    SECTION("sticky .Trash on the volume: .Trash/$uid") {
        stdfs::create_directories(vol / ".Trash");
        ::chmod((vol / ".Trash").c_str(), 01777);
        auto item = trash->trash(item_path);
        REQUIRE(item.has_value());
        CHECK(item->dir.kind == TrashDirKind::SharedUser);
        CHECK(item->dir.root == vol / ".Trash" / u);
        CHECK(read_text(item->info_path()).find("Path=Belgeler/") != std::string::npos);
    }
    SECTION("cross-filesystem trashing is refused: topdir trashes disabled") {
        auto o = fixture_options(fx.path(), table);
        o.use_topdir_trashes = false;
        auto t = make_fixture_trash(o);
        CHECK(t->trash(item_path).error().code == ErrorCode::Unsupported);
        CHECK(t->trash_dir_for(item_path).error().code == ErrorCode::Unsupported);
        CHECK(exists_nofollow(item_path));
        CHECK_FALSE(exists_nofollow(fx / "data"));
        CHECK(count_entries(vol) == 1);
    }
    SECTION("cross-filesystem trashing is refused: no mount for the item") {
        auto t = make_fixture_trash(fixture_options(fx.path(), mount_line(41, fx / "elsewhere")));
        CHECK(t->trash(item_path).error().code == ErrorCode::Unsupported);
        CHECK(exists_nofollow(item_path));
        CHECK(count_entries(vol) == 1);
    }
    SECTION("the longest matching mount point is the topdir") {
        auto outer = make_fixture_trash(fixture_options(fx.path(), mount_line(42, shm.path())));
        auto d = outer->trash_dir_for(item_path);
        REQUIRE(d.has_value());
        CHECK(d->topdir == shm.path());
        auto nested = make_fixture_trash(
            fixture_options(fx.path(), mount_line(43, shm.path()) + mount_line(44, vol)));
        CHECK(nested->trash_dir_for(item_path).value().topdir == vol);
        CHECK(count_entries(vol) == 1);
    }
    SECTION("read-only mount in the table: ReadOnlyFilesystem") {
        auto t = make_fixture_trash(fixture_options(fx.path(), mount_line(45, vol, "ext4", "ro")));
        CHECK(t->trash(item_path).error().code == ErrorCode::ReadOnlyFilesystem);
        CHECK(exists_nofollow(item_path));
        CHECK(count_entries(vol) == 1);
    }
    SECTION("the volume's mount point itself is refused") {
        CHECK(trash->trash(vol).error().code == ErrorCode::InvalidArgument);
        CHECK(exists_nofollow(vol));
    }
    SECTION("unusable volume trash: Unsupported, never the home trash") {
        stdfs::create_directories(fx / "t");
        stdfs::create_directory_symlink(fx / "t", vol / (".Trash-" + u));
        CHECK(trash->trash(item_path).error().code == ErrorCode::Unsupported);
        CHECK(exists_nofollow(item_path));
        CHECK_FALSE(exists_nofollow(fx / "data"));
        CHECK(count_entries(fx / "t") == 0);
    }
}

TEST_CASE("trash interop: GIO writes into a fixture home trash, Navigator reads it",
          "[trash][interop]") {
    auto gio = navigator::platform::find_program("gio");
    if (!gio) {
        SKIP("gio is not installed");
    }
    FixtureDir fx;
    const Path home = fx / "home";
    const Path data = fx / "data";
    stdfs::create_directories(home);
    stdfs::create_directories(data);
    const Path src = fx / "Masaüstü" / "a b ğ%+.txt";
    write_file(src, "gio");
    require_in_fixture(src);
    require_in_fixture(home);
    require_in_fixture(data);

    // Local VFS only (no gvfsd, no D-Bus), HOME and XDG_DATA_HOME both inside the fixture:
    // g_local_file_trash() then picks $XDG_DATA_HOME/Trash = <fixture>/data/Trash.
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        const std::string home_env = "HOME=" + home.native();
        const std::string data_env = "XDG_DATA_HOME=" + data.native();
        const char* envp[] = {home_env.c_str(),
                              data_env.c_str(),
                              "GIO_USE_VFS=local",
                              "GIO_USE_VOLUME_MONITOR=unix",
                              "PATH=/usr/bin:/bin",
                              "LC_ALL=C.UTF-8",
                              nullptr};
        const char* argv[] = {"gio", "trash", src.c_str(), nullptr};
        ::execve(gio->c_str(), const_cast<char* const*>(argv), const_cast<char* const*>(envp));
        ::_exit(127);
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || exists_nofollow(src)) {
        SKIP("gio trash did not run against the fixture");
    }
    REQUIRE(exists_nofollow(data / "Trash" / "info" / "a b ğ%+.txt.trashinfo"));

    auto o = fixture_options(fx.path());
    o.home_trash = data / "Trash";
    auto trash = make_fixture_trash(o);
    auto listing = trash->list(std::stop_token{});
    REQUIRE(listing.has_value());
    REQUIRE(listing->items.size() == 1);
    const TrashedItem& item = listing->items[0];
    require_in_fixture(item);
    CHECK(item.name == "a b ğ%+.txt");
    CHECK(item.original_path == src);
    CHECK(item.deletion_date.has_value());
    CHECK(listing->orphans == 0);

    // Navigator writes the same bytes GIO wrote, apart from the date.
    const std::string gio_text = read_text(item.info_path());
    const std::string ours = navigator::trash::format_trashinfo(
        navigator::trash::TrashInfo{src.native(), item.deletion_date});
    CHECK(gio_text == ours);

    REQUIRE(trash->restore(item, src).has_value());
    CHECK(read_text(src) == "gio");
    // `gio trash --list` cannot be pointed at a fixture (it needs gvfsd-trash, which watches
    // the session's real trash), so the reverse direction is not run; see
    // docs/platform-limitations.md.
}

TEST_CASE("move_to_trash wrapper uses the XDG home trash", "[trash]") {
    FixtureDir fx;
    ScopedEnv data("XDG_DATA_HOME", (fx / "xdg").native());
    require_in_fixture(fx / "xdg" / "Trash");
    CHECK(navigator::platform::move_to_trash(fx / "missing").error().code == ErrorCode::NotFound);
    CHECK_FALSE(exists_nofollow(fx / "xdg"));
    write_file(fx / "doomed", "d");
    REQUIRE(navigator::platform::move_to_trash(fx / "doomed").has_value());
    CHECK_FALSE(exists_nofollow(fx / "doomed"));
    CHECK(read_text(fx / "xdg" / "Trash" / "files" / "doomed") == "d");
    CHECK(exists_nofollow(fx / "xdg" / "Trash" / "info" / "doomed.trashinfo"));
}
