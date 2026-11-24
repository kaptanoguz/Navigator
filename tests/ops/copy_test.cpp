// Copy (S6, S10, S11, S13, S14): happy paths, metadata, special files, refusals. Fixtures only.
#include <array>
#include <cerrno>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;

namespace {

void set_mtime(const Path& p, std::int64_t sec) {
    require_inside_fixture(p);
    const std::array<struct timespec, 2> t{timespec{sec, 0}, timespec{sec, 123456789}};
    REQUIRE(::utimensat(AT_FDCWD, p.c_str(), t.data(), AT_SYMLINK_NOFOLLOW) == 0);
}

std::int64_t mtime_ns(const Path& p) {
    const struct stat st = lstat_of(p);
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
}

} // namespace

TEST_CASE("copy a file: content, mode, times, undo record, no temp left", "[copy]") {
    Fixture fx;
    Env env(fx);
    const Path src = fx.file("src/a.txt", "hello world");
    ::chmod(src.c_str(), 0640);
    set_mtime(src, 1000000000);
    const Path dst = fx.dir("dst");
    auto r = run(env, copy_req({src}, dst));
    REQUIRE(r);
    CHECK(r->done == 1);
    CHECK(r->failed == 0);
    REQUIRE(r->items.size() == 1);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(r->items[0].destination == dst / "a.txt");
    CHECK(read_file(dst / "a.txt") == "hello world");
    CHECK((lstat_of(dst / "a.txt").st_mode & 07777) == 0640);
    CHECK(mtime_ns(dst / "a.txt") == mtime_ns(src));
    CHECK(r->bytes_done == 11);
    CHECK(part_files(fx.root()).empty());
    REQUIRE(r->record);
    CHECK(record_of(*r).kind == OpKind::Copy);
    REQUIRE(record_of(*r).steps.size() == 1);
    const UndoStep& s = record_of(*r).steps[0];
    CHECK(s.kind == OpKind::Copy);
    CHECK(s.before == src);
    CHECK(s.after.path == dst / "a.txt");
    CHECK(s.after.kind == fs::EntryKind::File);
    CHECK(s.after.size == 11);
    CHECK_FALSE(record_of(*r).lossy);
    CHECK_FALSE(r->touched_folders.empty());
    CHECK(read_file(src) == "hello world"); // source untouched
}

TEST_CASE("copy a tree: folders, links, fifo, read-only folder, folder times", "[copy]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/t/a.txt", "A");
    fx.file("src/t/sub/b.bin", std::string(300000, 'b'));
    fx.dir("src/t/empty");
    fx.link("src/t/rel-link", "sub/b.bin");
    fx.link("src/t/abs-link", (fx / "src/t/a.txt").native());
    fx.link("src/t/dangling", "does/not/exist");
    REQUIRE(::mkfifo((fx / "src/t/pipe").c_str(), 0640) == 0);
    fx.file("src/t/ro/inside.txt", "ro");
    ::chmod((fx / "src/t/ro").c_str(), 0555);
    set_mtime(fx / "src/t/sub", 1200000000);
    set_mtime(fx / "src/t", 1300000000);
    const Path dst = fx.dir("dst");
    auto r = run(env, copy_req({fx / "src/t"}, dst));
    REQUIRE(r);
    CHECK(r->done == 1);
    CHECK_FALSE(r->items[0].partial);
    CHECK(tree_digest(dst / "t") == tree_digest(fx / "src/t"));
    struct stat pipe = lstat_of(dst / "t/pipe");
    CHECK(S_ISFIFO(pipe.st_mode));
    CHECK((pipe.st_mode & 0777) == 0640);
    CHECK(mtime_ns(dst / "t/sub") == mtime_ns(fx / "src/t/sub"));
    CHECK(mtime_ns(dst / "t") == mtime_ns(fx / "src/t"));
    CHECK((lstat_of(dst / "t/ro").st_mode & 0777) == 0555);
    CHECK(read_file(dst / "t/ro/inside.txt") == "ro");
    CHECK(part_files(fx.root()).empty());
    // The link was copied as a link, pointing at the same text (S4).
    CHECK(S_ISLNK(lstat_of(dst / "t/abs-link").st_mode));
    REQUIRE(r->record);
    CHECK(record_of(*r).steps.size() == 1); // one step for the whole new tree
}

TEST_CASE("sockets and devices are not copied (Failed Unsupported); the rest is", "[copy]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/t/a.txt", "A");
    const Path sock = fx / "src/t/sock";
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sock.native().size() >= sizeof addr.sun_path) {
        ::close(fd);
        SKIP("fixture path too long for a unix socket");
    }
    std::strncpy(addr.sun_path, sock.c_str(), sizeof addr.sun_path - 1);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    ::close(fd);
    const Path dst = fx.dir("dst");
    auto r = run(env, copy_req({fx / "src/t"}, dst));
    REQUIRE(r);
    REQUIRE(r->items.size() == 1);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(r->items[0].partial);
    REQUIRE(r->items[0].error);
    CHECK(code_of(r->items[0]) == ErrorCode::Unsupported);
    CHECK(read_file(dst / "t/a.txt") == "A");
    CHECK_FALSE(present(dst / "t/sock"));
}

TEST_CASE("hard links inside a copied tree are linked again", "[copy]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("src/t/a.txt", "shared");
    REQUIRE(::link(a.c_str(), (fx / "src/t/sub/../b.txt").lexically_normal().c_str()) == 0);
    fx.dir("src/t/sub");
    REQUIRE(::link(a.c_str(), (fx / "src/t/sub/c.txt").c_str()) == 0);
    const Path dst = fx.dir("dst");
    SECTION("preserve_hardlinks (default)") {
        auto r = run(env, copy_req({fx / "src/t"}, dst));
        REQUIRE(r);
        const struct stat sa = lstat_of(dst / "t/a.txt");
        const struct stat sb = lstat_of(dst / "t/b.txt");
        const struct stat sc = lstat_of(dst / "t/sub/c.txt");
        CHECK(sa.st_ino == sb.st_ino);
        CHECK(sa.st_ino == sc.st_ino);
        CHECK(sa.st_nlink == 3);
        CHECK(sa.st_ino != lstat_of(a).st_ino);
    }
    SECTION("preserve_hardlinks off") {
        OpRequest req = copy_req({fx / "src/t"}, dst);
        req.options.preserve_hardlinks = false;
        auto r = run(env, req);
        REQUIRE(r);
        CHECK(lstat_of(dst / "t/a.txt").st_ino != lstat_of(dst / "t/b.txt").st_ino);
        CHECK(read_file(dst / "t/b.txt") == "shared");
    }
}

TEST_CASE("sparse files keep their holes", "[copy]") {
    Fixture fx;
    Env env(fx);
    const Path src = fx / "src.img";
    fx.dir("d");
    {
        const int fd = ::open(src.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        REQUIRE(fd >= 0);
        REQUIRE(::ftruncate(fd, 64L << 20) == 0);
        const std::string mid(4096, 'm');
        REQUIRE(::pwrite(fd, mid.data(), mid.size(), 1L << 20) == 4096);
        REQUIRE(::pwrite(fd, mid.data(), mid.size(), (64L << 20) - 4096) == 4096);
        ::close(fd);
    }
    const struct stat sst = lstat_of(src);
    if (static_cast<std::uint64_t>(sst.st_blocks) * 512U >=
        static_cast<std::uint64_t>(sst.st_size)) {
        SKIP("the fixture filesystem does not create sparse files");
    }
    auto r = run(env, copy_req({src}, fx / "d"));
    REQUIRE(r);
    CHECK(r->done == 1);
    const struct stat dst = lstat_of(fx / "d/src.img");
    CHECK(dst.st_size == sst.st_size);
    CHECK(static_cast<std::uint64_t>(dst.st_blocks) * 512U < (4UL << 20)); // holes kept
    CHECK(read_file(fx / "d/src.img") == read_file(src));
    CHECK(r->bytes_done == static_cast<std::uint64_t>(sst.st_size));
}

TEST_CASE("user.* xattrs are copied best effort", "[copy]") {
    Fixture fx;
    Env env(fx);
    const Path f = fx.file("src/t/x.txt", "x");
    const std::string v = "değer";
    if (::setxattr(f.c_str(), "user.navigator.test", v.data(), v.size(), 0) != 0) {
        SKIP("user xattrs unsupported on the fixture filesystem");
    }
    REQUIRE(::setxattr((fx / "src/t").c_str(), "user.dir", "1", 1, 0) == 0);
    auto r = run(env, copy_req({fx / "src/t"}, fx.dir("dst")));
    REQUIRE(r);
    std::array<char, 64> buf{};
    const ssize_t n =
        ::getxattr((fx / "dst/t/x.txt").c_str(), "user.navigator.test", buf.data(), buf.size());
    REQUIRE(n == static_cast<ssize_t>(v.size()));
    CHECK(std::string(buf.data(), static_cast<std::size_t>(n)) == v);
    CHECK(::getxattr((fx / "dst/t").c_str(), "user.dir", buf.data(), buf.size()) == 1);
    CHECK(r->metadata_warnings == 0);

    SECTION("preserve_xattrs off") {
        OpRequest req = copy_req({fx / "src/t"}, fx.dir("dst2"));
        req.options.preserve_xattrs = false;
        auto r2 = run(env, req);
        REQUIRE(r2);
        CHECK(::getxattr((fx / "dst2/t/x.txt").c_str(), "user.navigator.test", buf.data(),
                         buf.size()) < 0);
    }
}

TEST_CASE("mode bits: sticky and setgid kept for the same owner; preserve_mode off", "[copy]") {
    Fixture fx;
    Env env(fx);
    const Path d = fx.dir("src/sticky");
    ::chmod(d.c_str(), 01775);
    const Path f = fx.file("src/f.sh", "#!/bin/sh\n");
    ::chmod(f.c_str(), 0755);
    set_mtime(f, 1000000000);
    auto r = run(env, copy_req({d, f}, fx.dir("dst")));
    REQUIRE(r);
    CHECK((lstat_of(fx / "dst/sticky").st_mode & 07777) == 01775);
    CHECK((lstat_of(fx / "dst/f.sh").st_mode & 07777) == 0755);
    OpRequest plain = copy_req({f}, fx.dir("dst2"));
    plain.options.preserve_mode = false;
    plain.options.preserve_times = false;
    auto r2 = run(env, plain);
    REQUIRE(r2);
    const mode_t expect = 0666 & ~ops::detail::process_umask();
    CHECK((lstat_of(fx / "dst2/f.sh").st_mode & 07777) == expect);
    CHECK(mtime_ns(fx / "dst2/f.sh") != mtime_ns(f));
}

TEST_CASE("copy into the sources' own folder always keeps both", "[copy][conflict]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("d/a.txt", "1");
    const Path folder = fx.dir("d/sub");
    fx.file("d/sub/in.txt", "in");
    for (ConflictPolicy p : {ConflictPolicy::Ask, ConflictPolicy::Replace, ConflictPolicy::Skip}) {
        Answer ans({ConflictAction::Replace, false, {}});
        auto r = run(env, copy_req({a, folder}, fx / "d", p), ans.resolver());
        REQUIRE(r);
        CHECK(r->done == 2);
        CHECK(ans.seen.empty()); // never asked
    }
    CHECK(read_file(fx / "d/a (2).txt") == "1");
    CHECK(read_file(fx / "d/a (3).txt") == "1");
    CHECK(read_file(fx / "d/a (4).txt") == "1");
    CHECK(read_file(fx / "d/sub (2)/in.txt") == "in");
    CHECK(read_file(fx / "d/sub (4)/in.txt") == "in");
    CHECK(read_file(a) == "1");
}

TEST_CASE("copy of a folder into itself is refused", "[copy][refuse]") {
    Fixture fx;
    Env env(fx);
    const Path d = fx.dir("d");
    fx.dir("d/inner");
    auto r = run(env, copy_req({d}, fx / "d/inner"));
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    auto r2 = run(env, copy_req({d}, d));
    REQUIRE_FALSE(r2);
    // Through a symlinked destination (lexically elsewhere): refused at execution time.
    fx.link("alias", (fx / "d/inner").native());
    auto r3 = run(env, copy_req({d}, fx / "alias"));
    REQUIRE(r3);
    REQUIRE(r3->items.size() == 1);
    CHECK(r3->items[0].status == ItemStatus::Failed);
    CHECK(code_of(r3->items[0]) == ErrorCode::InvalidArgument);
    CHECK(count_entries(d) == 1);
}

TEST_CASE("request validation (S3, S13)", "[refuse]") {
    Fixture fx;
    Env env(fx);
    const Path f = fx.file("f", "x");
    OpRequest rel = copy_req({"relative/f"}, fx.root());
    CHECK(run_operation(rel, env.engine, {}, {}, {}).error().code == ErrorCode::InvalidArgument);
    OpRequest dotdot = copy_req({fx / "a/../f"}, fx.root());
    CHECK(run_operation(dotdot, env.engine, {}, {}, {}).error().code == ErrorCode::InvalidArgument);
    CHECK(run(env, copy_req({}, fx.root())).error().code == ErrorCode::InvalidArgument);
    OpRequest ren = rename_req(f, "a/b");
    CHECK(run(env, ren).error().code == ErrorCode::InvalidArgument);
    ren.new_name = "..";
    CHECK(run(env, ren).error().code == ErrorCode::InvalidArgument);
    OpRequest two = rename_req(f, "g");
    two.sources.push_back(fx / "other");
    CHECK(run(env, two).error().code == ErrorCode::InvalidArgument);
    OpRequest del = delete_req({f});
    del.confirmed = false;
    CHECK(run(env, del).error().code == ErrorCode::InvalidArgument);
    CHECK(present(f));
    OpRequest undo;
    undo.kind = OpKind::Undo;
    CHECK(run(env, undo).error().code == ErrorCode::InvalidArgument);
    CHECK(run(env, mkdir_req(fx.root(), "")).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("dereference_top_level copies a link's target folder", "[copy]") {
    Fixture fx;
    Env env(fx);
    fx.file("real/x.txt", "x");
    const Path l = fx.link("lnk", (fx / "real").native());
    OpRequest req = copy_req({l}, fx.dir("dst"));
    req.options.dereference_top_level = true;
    auto r = run(env, req);
    REQUIRE(r);
    CHECK(r->done == 1);
    CHECK(S_ISDIR(lstat_of(fx / "dst/lnk").st_mode));
    CHECK(read_file(fx / "dst/lnk/x.txt") == "x");
    // Default: the link itself.
    auto r2 = run(env, copy_req({l}, fx.dir("dst2")));
    REQUIRE(r2);
    CHECK(S_ISLNK(lstat_of(fx / "dst2/lnk").st_mode));
}

TEST_CASE("free space is checked before anything is written (S14)", "[copy][space]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/big", std::string(100000, 'z'));
    fx.file("src/small", "s");
    const Path dst = fx.dir("dst");
    const std::string before = tree_digest(dst);
    {
        ScopedFreeSpace low(1000);
        auto r = run(env, copy_req({fx / "src/big", fx / "src/small"}, dst));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::NoSpace);
    }
    CHECK(tree_digest(dst) == before);
    CHECK(part_files(fx.root()).empty());
}

TEST_CASE("dry_run plans without touching anything", "[copy][dry]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/a", "1");
    fx.file("src/b", "2");
    fx.file("dst/b", "old");
    const std::string before = tree_digest(fx.root());
    OpRequest req = copy_req({fx / "src/a", fx / "src/b"}, fx / "dst");
    req.options.dry_run = true;
    Answer ans({ConflictAction::Replace, false, {}});
    auto r = run(env, req, ans.resolver());
    REQUIRE(r);
    REQUIRE(r->items.size() == 2);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(r->items[1].status == ItemStatus::Conflict);
    CHECK(ans.seen.empty());
    CHECK_FALSE(r->record);
    CHECK(tree_digest(fx.root()) == before);
    {
        ScopedFreeSpace low(0);
        req.sources = {fx / "src/a"};
        auto r2 = run(env, req);
        REQUIRE_FALSE(r2);
        CHECK(r2.error().code == ErrorCode::NoSpace);
    }
}

TEST_CASE("missing and unreadable sources fail per item, others succeed", "[copy]") {
    Fixture fx;
    Env env(fx);
    const Path ok_src = fx.file("src/ok", "ok");
    const Path unreadable = fx.file("src/secret", "s");
    ::chmod(unreadable.c_str(), 0);
    const Path dst = fx.dir("dst");
    auto r = run(env, copy_req({fx / "src/missing", ok_src, unreadable}, dst));
    REQUIRE(r);
    REQUIRE(r->items.size() == 3);
    CHECK(r->items[0].status == ItemStatus::Failed);
    CHECK(code_of(r->items[0]) == ErrorCode::NotFound);
    CHECK(r->items[1].status == ItemStatus::Done);
    if (::geteuid() != 0) {
        CHECK(r->items[2].status == ItemStatus::Failed);
        CHECK(code_of(r->items[2]) == ErrorCode::PermissionDenied);
        CHECK_FALSE(present(dst / "secret"));
    }
    CHECK(r->failed == (::geteuid() != 0 ? 2U : 1U));
    CHECK(part_files(fx.root()).empty());
}

TEST_CASE("progress reports totals, bytes and phases", "[copy][progress]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/t/a", std::string(200000, 'a'));
    fx.file("src/t/b", std::string(100000, 'b'));
    env.engine.progress_interval = std::chrono::milliseconds(0);
    std::vector<OpProgress> seen;
    auto r = run(env, copy_req({fx / "src/t"}, fx.dir("dst")), {}, {}, {},
                 [&](const OpProgress& p) { seen.push_back(p); });
    REQUIRE(r);
    REQUIRE_FALSE(seen.empty());
    const OpProgress& last = seen.back();
    CHECK(last.phase == OpPhase::Done);
    CHECK(last.totals_final);
    CHECK(last.bytes_total == 300000);
    CHECK(last.bytes_done == 300000);
    CHECK(last.items_total == 3);
    CHECK(last.items_done == 3);
    CHECK(last.top_level_done == 1);
    CHECK(last.top_level_total == 1);
    bool saw_scanning = false;
    bool saw_working = false;
    std::uint64_t prev = 0;
    for (const OpProgress& p : seen) {
        saw_scanning = saw_scanning || p.phase == OpPhase::Scanning;
        saw_working = saw_working || p.phase == OpPhase::Working;
        CHECK(p.bytes_done >= prev); // monotonic
        prev = p.bytes_done;
        CHECK(p.bytes_per_second == 0.0); // under 2 s: no rate yet
        CHECK_FALSE(p.eta.has_value());
    }
    CHECK(saw_scanning);
    CHECK(saw_working);
}

TEST_CASE("rate and ETA appear after 2 s of stable samples", "[copy][progress][slow]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/slow.bin", std::string(std::size_t{48} * 65536, 's'));
    env.engine.progress_interval = std::chrono::milliseconds(50);
    ScopedIoHook hook([](ops::detail::IoSite site, const Path&, std::uint64_t) -> int {
        if (site == ops::detail::IoSite::Write) {
            ::usleep(60000); // 48 chunks x 60 ms ~ 2.9 s
        }
        return 0;
    });
    std::vector<OpProgress> seen;
    auto r = run(env, copy_req({fx / "src/slow.bin"}, fx.dir("dst")), {}, {}, {},
                 [&](const OpProgress& p) { seen.push_back(p); });
    REQUIRE(r);
    bool rate = false;
    bool eta = false;
    for (const OpProgress& p : seen) {
        if (p.elapsed < std::chrono::milliseconds(2000)) {
            CHECK(p.bytes_per_second == 0.0);
        }
        rate = rate || p.bytes_per_second > 0.0;
        eta = eta || p.eta.has_value();
    }
    CHECK(rate);
    CHECK(eta);
}
