// Move and Rename (S7, S13, S15), symlinks never followed (S4), cross-filesystem moves with
// verification. Fixtures only (a /dev/shm fixture for the real cross-filesystem case).
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;

TEST_CASE("same-filesystem move keeps the inode and records the step", "[move]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("src/a.txt", "a");
    fx.file("src/d/x", "x");
    const ino_t ino = lstat_of(a).st_ino;
    const Path dst = fx.dir("dst");
    auto r = run(env, move_req({a, fx / "src/d"}, dst));
    REQUIRE(r);
    CHECK(r->done == 2);
    CHECK(lstat_of(dst / "a.txt").st_ino == ino);
    CHECK(read_file(dst / "d/x") == "x");
    CHECK_FALSE(present(a));
    REQUIRE(r->record);
    CHECK(record_of(*r).kind == OpKind::Move);
    REQUIRE(record_of(*r).steps.size() == 2);
    CHECK(record_of(*r).steps[0].before == a);
    CHECK(record_of(*r).steps[0].after.path == dst / "a.txt");
    CHECK(record_of(*r).steps[0].after.id.inode == ino);
    CHECK(record_of(*r).destination == dst);
}

TEST_CASE("a move into the item's own folder is skipped", "[move]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("d/a", "a");
    auto r = run(env, move_req({a}, fx / "d"));
    REQUIRE(r);
    CHECK(r->items[0].status == ItemStatus::Skipped);
    CHECK(read_file(a) == "a");
}

TEST_CASE("protected sources are refused (S13) for move, rename, trash, delete", "[move][refuse]") {
    Fixture fx;
    Env env(fx);
    const Path home = fx.dir("home/user");
    const Path mnt = fx.dir("mnt");
    fx.file("home/user/doc", "d");
    env.engine.protected_paths.home = home;
    env.engine.protected_paths.mount_points = {mnt};
    const Path dst = fx.dir("dst");
    for (const Path& p : {home, fx / "home", mnt, env.trash->root() / "files"}) {
        CAPTURE(p.native());
        CHECK(run(env, move_req({p}, dst)).error().code == ErrorCode::PermissionDenied);
        CHECK(run(env, rename_req(p, "zz")).error().code == ErrorCode::PermissionDenied);
        CHECK(run(env, trash_req({p})).error().code == ErrorCode::PermissionDenied);
        CHECK(run(env, delete_req({p})).error().code == ErrorCode::PermissionDenied);
    }
    // Copying protected paths is allowed.
    auto c = run(env, copy_req({home}, dst));
    REQUIRE(c);
    CHECK(read_file(dst / "user/doc") == "d");
    // Through a symlinked parent the canonical location is checked at execution time (S15).
    fx.link("alias", (fx / "home").native());
    auto r = run(env, move_req({fx / "alias/user"}, dst));
    REQUIRE(r);
    CHECK(r->items[0].status == ItemStatus::Failed);
    CHECK(code_of(r->items[0]) == ErrorCode::PermissionDenied);
    CHECK(read_file(home / "doc") == "d");
}

TEST_CASE("symlinks are moved as links; the sentinel they point at stays untouched",
          "[move][symlink]") {
    Fixture fx;
    Fixture sentinel_fx; // "outside": a separate fixture the operated tree never contains
    Env env(fx);
    sentinel_fx.file("sentinel/keep.txt", "precious");
    sentinel_fx.file("sentinel/sub/deep.txt", "deep");
    const std::string before = tree_digest(sentinel_fx.root());
    fx.file("src/t/own.txt", "own");
    fx.link("src/t/to-sentinel", (sentinel_fx / "sentinel").native());
    fx.link("src/top-link", (sentinel_fx / "sentinel").native());
    const Path dst = fx.dir("dst");
    auto r = run(env, move_req({fx / "src/t", fx / "src/top-link"}, dst));
    REQUIRE(r);
    CHECK(r->done == 2);
    CHECK(S_ISLNK(lstat_of(dst / "t/to-sentinel").st_mode));
    CHECK(S_ISLNK(lstat_of(dst / "top-link").st_mode));
    CHECK(tree_digest(sentinel_fx.root()) == before);
}

TEST_CASE("rename: plain, conflict, case-only and unchanged name", "[rename]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("d/a.txt", "a");
    fx.file("d/b.txt", "b");
    auto r = run(env, rename_req(a, "c.txt"));
    REQUIRE(r);
    CHECK(r->done == 1);
    CHECK(read_file(fx / "d/c.txt") == "a");
    REQUIRE(r->record);
    CHECK(record_of(*r).kind == OpKind::Rename);
    CHECK(record_of(*r).steps[0].kind == OpKind::Rename);
    CHECK(record_of(*r).destination == fx / "d");

    auto same = run(env, rename_req(fx / "d/c.txt", "c.txt"));
    REQUIRE(same);
    CHECK(same->items[0].status == ItemStatus::Skipped);

    Answer ans({ConflictAction::KeepBoth, false, {}});
    auto taken = run(env, rename_req(fx / "d/c.txt", "b.txt"), ans.resolver());
    REQUIRE(taken);
    REQUIRE(ans.seen.size() == 1);
    CHECK(read_file(fx / "d/b (2).txt") == "a");
    CHECK(read_file(fx / "d/b.txt") == "b");

    // Case-only rename (on a case-sensitive fixture it is an ordinary rename).
    auto upper = run(env, rename_req(fx / "d/b.txt", "B.txt"));
    REQUIRE(upper);
    CHECK(upper->done == 1);
    CHECK(read_file(fx / "d/B.txt") == "b");

    // Turkish and invalid UTF-8 names.
    auto tr = run(env, rename_req(fx / "d/B.txt", "İstanbul ığüşöç \xff.txt"));
    REQUIRE(tr);
    CHECK(read_file(fx / "d" / std::string("İstanbul ığüşöç \xff.txt")) == "b");
}

TEST_CASE("cross-filesystem move (tmpfs fixture): copy, verify, remove source", "[move][xfs]") {
    if (!Fixture::shm_is_other_filesystem()) {
        SKIP("/dev/shm is not a separate writable filesystem here");
    }
    Fixture fx;
    Fixture shm(Fixture::Where::Shm);
    Env env(fx);
    fx.file("src/t/a.txt", "a");
    fx.file("src/t/sub/b.bin", std::string(500000, 'b'));
    fx.link("src/t/link", "sub/b.bin");
    fx.dir("src/t/empty");
    const std::string digest = tree_digest(fx / "src/t");
    auto r = run(env, move_req({fx / "src/t"}, shm.root()));
    REQUIRE(r);
    CHECK(r->done == 1);
    CHECK_FALSE(r->items[0].partial);
    CHECK(tree_digest(shm / "t") == digest);
    CHECK_FALSE(present(fx / "src/t"));
    CHECK(part_files(shm.root()).empty());
    REQUIRE(r->record);
    CHECK(record_of(*r).steps.size() == 1);
    CHECK(record_of(*r).steps[0].kind == OpKind::Move);
    // Undo moves it back across the filesystems again.
    auto u = run(env, undo_req(record_of(*r)));
    REQUIRE(u);
    CHECK(u->done == 1);
    CHECK(tree_digest(fx / "src/t") == digest);
    CHECK_FALSE(present(shm / "t"));
    // Redo once more.
    REQUIRE(u->record);
    auto again = run(env, redo_req(record_of(*u)));
    REQUIRE(again);
    CHECK(again->done == 1);
    CHECK(tree_digest(shm / "t") == digest);
}

TEST_CASE("cross-filesystem move via the EXDEV seam: a source changed mid-copy stays",
          "[move][xfs][verify]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/t/stable.txt", "stable");
    const Path changing = fx.file("src/t/changing.txt", std::string(300000, 'c'));
    fx.file("src/t/z-last.txt", "z");
    const Path dst = fx.dir("dst");
    bool appended = false;
    ScopedIoHook hook([&](ops::detail::IoSite site, const Path& p, std::uint64_t off) -> int {
        if (site == ops::detail::IoSite::Rename) {
            return EXDEV; // pretend dst is another filesystem
        }
        if (site == ops::detail::IoSite::Write && p.filename() == "changing.txt" && off > 0 &&
            !appended) {
            appended = true; // another program appends while we copy
            const int fd = ::open(changing.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
            REQUIRE(fd >= 0);
            REQUIRE(::write(fd, "more", 4) == 4);
            ::close(fd);
        }
        return 0;
    });
    auto r = run(env, move_req({fx / "src/t"}, dst));
    REQUIRE(r);
    REQUIRE(appended);
    REQUIRE(r->items.size() == 1);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(r->items[0].partial);
    // The changed file stays at the source (with its new content), not at the destination.
    CHECK(read_file(changing).size() == 300004);
    CHECK_FALSE(present(dst / "t/changing.txt"));
    // The verified files were moved; the source folder stays because it is not empty.
    CHECK(read_file(dst / "t/stable.txt") == "stable");
    CHECK_FALSE(present(fx / "src/t/stable.txt"));
    CHECK(present(fx / "src/t"));
    CHECK(part_files(fx.root()).empty());
}

TEST_CASE("cross-filesystem move: a destination write failure rolls the item back",
          "[move][xfs][fault]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/t/a.txt", "a");
    fx.file("src/t/b/c.bin", std::string(200000, 'c'));
    fx.file("src/u.txt", "u");
    const std::string digest = tree_digest(fx / "src");
    const Path dst = fx.dir("dst");
    for (int err : {ENOSPC, EIO}) {
        CAPTURE(err);
        ScopedIoHook hook([&](ops::detail::IoSite site, const Path& p, std::uint64_t off) -> int {
            if (site == ops::detail::IoSite::Rename) {
                return EXDEV;
            }
            if (site == ops::detail::IoSite::Write && p.filename() == "c.bin" && off >= 65536) {
                return err;
            }
            return 0;
        });
        auto r = run(env, move_req({fx / "src/t", fx / "src/u.txt"}, dst));
        REQUIRE(r);
        REQUIRE(r->items.size() == 2);
        CHECK(r->items[0].status == ItemStatus::Failed);
        CHECK(code_of(r->items[0]) == (err == ENOSPC ? ErrorCode::NoSpace : ErrorCode::IoError));
        CHECK_FALSE(present(dst / "t")); // partial destination removed (S1d)
        CHECK(r->items[1].status == ItemStatus::Done);
        CHECK(part_files(fx.root()).empty());
        // Put u.txt back for the next round; the failed tree never left.
        REQUIRE(::rename((dst / "u.txt").c_str(), (fx / "src/u.txt").c_str()) == 0);
        CHECK(tree_digest(fx / "src") == digest);
    }
}

TEST_CASE("move refuses a folder into itself, also through a link", "[move][refuse]") {
    Fixture fx;
    Env env(fx);
    fx.file("d/in/x", "x");
    CHECK(run(env, move_req({fx / "d"}, fx / "d/in")).error().code == ErrorCode::InvalidArgument);
    fx.link("l", (fx / "d/in").native());
    auto r = run(env, move_req({fx / "d"}, fx / "l"));
    REQUIRE(r);
    CHECK(r->items[0].status == ItemStatus::Failed);
    CHECK(code_of(r->items[0]) == ErrorCode::InvalidArgument);
    CHECK(read_file(fx / "d/in/x") == "x");
}

TEST_CASE("cross-device restore copies out of the trash, erases it and forgets the info",
          "[restore][xfs]") {
    if (!Fixture::shm_is_other_filesystem()) {
        SKIP("/dev/shm is not a separate writable filesystem here");
    }
    Fixture fx;
    Fixture shm(Fixture::Where::Shm);
    Env env(fx);
    fx.file("t/x/inner.txt", "inner");
    auto t = run(env, trash_req({fx / "t/x"}));
    REQUIRE(t);
    trash::TrashedItem item = trashed_of(record_of(*t).steps[0]);
    // As if another program had trashed it from the tmpfs volume into this trash.
    item.original_path = shm / "restored/x";
    OpRequest req;
    req.kind = OpKind::Restore;
    req.trashed = {item};
    auto r = run(env, req);
    REQUIRE(r);
    REQUIRE(r->items.size() == 1);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(read_file(shm / "restored/x/inner.txt") == "inner");
    CHECK_FALSE(present(item.files_path()));
    CHECK_FALSE(present(item.info_path()));
    REQUIRE(r->record);
    CHECK(record_of(*r).steps[0].kind == OpKind::Restore);
    CHECK(record_of(*r).steps[0].created_parents.size() == 1);
}

TEST_CASE("cross-filesystem move keeps hard links of one tree and removes all names",
          "[move][xfs]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("src/t/a", "linked");
    REQUIRE(::link(a.c_str(), (fx / "src/t/b").c_str()) == 0);
    ScopedIoHook hook([](ops::detail::IoSite s, const Path&, std::uint64_t) -> int {
        return s == ops::detail::IoSite::Rename ? EXDEV : 0;
    });
    auto r = run(env, move_req({fx / "src/t"}, fx.dir("dst")));
    REQUIRE(r);
    CHECK(r->done == 1);
    CHECK_FALSE(r->items[0].partial);
    CHECK(lstat_of(fx / "dst/t/a").st_ino == lstat_of(fx / "dst/t/b").st_ino);
    CHECK_FALSE(present(fx / "src/t"));
}

TEST_CASE("cross-filesystem move: a source entry that appeared during the move stays",
          "[move][xfs][verify]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/t/a", std::string(200000, 'a'));
    bool added = false;
    ScopedIoHook hook([&](ops::detail::IoSite s, const Path& p, std::uint64_t) -> int {
        if (s == ops::detail::IoSite::Rename) {
            return EXDEV;
        }
        if (s == ops::detail::IoSite::Write && p.filename() == "a" && !added) {
            added = true;
            write_file(fx / "src/t/new-during-move", "n");
        }
        return 0;
    });
    auto r = run(env, move_req({fx / "src/t"}, fx.dir("dst")));
    REQUIRE(r);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(r->items[0].partial); // the folder stayed: not empty
    CHECK(read_file(fx / "src/t/new-during-move") == "n");
    CHECK_FALSE(present(fx / "src/t/a"));
    CHECK(read_file(fx / "dst/t/a").size() == 200000);
}
