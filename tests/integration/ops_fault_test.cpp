// Atomicity, cancellation and hostile-tree tests of the Phase 7 engine and queue (owner: qa).
// Fixtures only (ops_support.hpp aborts the process on any path outside them). Covers cancel
// storms at random points (entry counter, chunk counter and a timed stop from another thread,
// many seeds) for copy, same-filesystem move and cross-filesystem move (EXDEV seam), injected
// ENOSPC / EIO / EACCES, REAL ENOSPC on a 1 MiB tmpfs made in a private user+mount namespace,
// real read-only destinations, symlink traps (a sentinel tree that the engine must never name),
// hard links, FIFOs, sockets, a tree deeper than PATH_MAX, protected paths, copying a folder into
// itself through a link, 100k small files, and overlapping queue operations (TSan target).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "ops_support.hpp"

using namespace navigator;
using namespace navigator::opsit;
using navigator::ops::ConflictAction;
using navigator::ops::ConflictDecision;
using navigator::ops::ItemStatus;
using navigator::ops::OpKind;
using IoSite = navigator::ops::detail::IoSite;

namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

#if defined(__SANITIZE_THREAD__)
constexpr bool kTsan = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kTsan = true;
#else
constexpr bool kTsan = false;
#endif
#else
constexpr bool kTsan = false;
#endif

constexpr int kSeeds = kSanitized ? 24 : 60;

ops::ConflictResolver always(ConflictAction action) {
    return [action](const ops::Conflict&) {
        ConflictDecision d;
        d.action = action;
        return d;
    };
}

std::string random_bytes(std::mt19937& rng, std::size_t n) {
    std::string s(n, '\0');
    for (char& c : s) {
        c = static_cast<char>(rng() & 0xFFU);
    }
    return s;
}

// fx/<base>/{t0 dir tree, t1.bin 300 kB, t2 dir with links / hard links / FIFO, t3 small,
// t4 empty dir, t5 read-only file, "Türkçe ğ" dir}: about 1.2 MB, Turkish names included.
std::vector<Path> build_mixed(Fixture& fx, const std::string& base, unsigned seed,
                              bool special = true) {
    std::mt19937 rng(seed);
    const auto p = [&](const std::string& rel) { return base + "/" + rel; };
    fx.file(p("t0/a.txt"), random_bytes(rng, 100));
    fx.file(p("t0/sub1/b.bin"), random_bytes(rng, 150000));
    fx.file(p("t0/sub1/deeper/c.bin"), random_bytes(rng, 65537));
    fx.file(p("t0/sub2/d.bin"), random_bytes(rng, 400000));
    fx.file(p("t0/sub2/empty"), "");
    fx.dir(p("t0/sub3"));
    ::chmod(fx.dir(p("t0/sub1")).c_str(), 0750);
    fx.file(p("t1.bin"), random_bytes(rng, 300000));
    fx.file(p("t2/target.txt"), random_bytes(rng, 5000));
    fx.link(p("t2/rel-link"), "target.txt");
    fx.link(p("t2/dangling"), "/nonexistent/ğ");
    if (special) {
        const Path h1 = fx.file(p("t2/h1"), random_bytes(rng, 70000));
        REQUIRE(::link(h1.c_str(), (fx / p("t2/h2")).c_str()) == 0);
        fx.fifo(p("t2/pipe"));
    }
    fx.file(p("t3.txt"), "tiny");
    fx.dir(p("t4"));
    const Path ro = fx.file(p("t5.ro"), random_bytes(rng, 2000));
    ::chmod(ro.c_str(), 0444);
    fx.file(p("Türkçe ğ/İçerik şey.txt"), random_bytes(rng, 3000));
    return {fx / p("t0"), fx / p("t1.bin"), fx / p("t2"),      fx / p("t3.txt"),
            fx / p("t4"), fx / p("t5.ro"),  fx / p("Türkçe ğ")};
}

std::map<Path, std::string> digests_of(const std::vector<Path>& tops) {
    std::map<Path, std::string> d;
    for (const Path& t : tops) {
        d[t] = item_digest(t);
    }
    return d;
}

// Every regular file below `dst` equals its counterpart below `src`; every link has the same
// target (complete copies only: a truncated or half-written destination fails here).
void check_no_truncated(const Path& src, const Path& dst) {
    std::error_code ec;
    for (auto it = stdfs::recursive_directory_iterator(dst, ec);
         !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        const Path rel = it->path().lexically_relative(dst);
        const struct stat st = lstat_of(it->path());
        if (S_ISREG(st.st_mode)) {
            INFO("destination file " << rel.native());
            CHECK(hash_file(it->path()) == hash_file(src / rel));
            CHECK(st.st_size == lstat_of(src / rel).st_size);
        } else if (S_ISLNK(st.st_mode)) {
            CHECK(item_digest(it->path()) == item_digest(src / rel));
        }
    }
}

// The stop is requested from a timed thread, an entry counter or a write counter.
struct Storm {
    std::stop_source stop;
    std::atomic<int> counter{0};
    int target = 1;
    std::thread timer;
    ~Storm() {
        if (timer.joinable()) {
            timer.join();
        }
    }
};

void arm_storm(Rig& rig, Storm& s, unsigned seed, int mode) {
    std::mt19937 rng(seed * 7919U + 13U);
    switch (mode) {
    case 0:
        s.target = 1 + static_cast<int>(rng() % 28U);
        rig.user_inject = [&s](const Path&) {
            if (++s.counter == s.target) {
                s.stop.request_stop();
            }
            return true;
        };
        break;
    case 1:
        s.target = 1 + static_cast<int>(rng() % 14U);
        rig.user_io = [&s](IoSite site, const Path&, std::uint64_t) {
            if (site == IoSite::Write && ++s.counter == s.target) {
                s.stop.request_stop();
            }
            return 0;
        };
        break;
    default: {
        const auto delay = std::chrono::microseconds(rng() % 3000U);
        s.timer = std::thread([&s, delay] {
            std::this_thread::sleep_for(delay);
            s.stop.request_stop();
        });
        break;
    }
    }
}

void make_xdev(Rig& rig, std::function<int(IoSite, const Path&, std::uint64_t)> chained = {}) {
    auto inner = std::move(rig.user_io);
    rig.user_io = [inner, chained](IoSite site, const Path& p, std::uint64_t off) -> int {
        if (site == IoSite::Rename) {
            return EXDEV; // force the cross-filesystem path (copy, fsync, verify, remove)
        }
        if (inner) {
            if (const int e = inner(site, p, off); e != 0) {
                return e;
            }
        }
        return chained ? chained(site, p, off) : 0;
    };
}

// True when a private user+mount namespace with a tmpfs mount can be created here. Runs in a
// forked child (no Catch assertions: a SKIP after a passed assertion would make ctest report the
// test as Failed, not Skipped). NAVIGATOR_TEST_NO_USERNS=1 forces "no" (CI runners restrict it).
bool userns_tmpfs_usable() {
    if (const char* no = std::getenv("NAVIGATOR_TEST_NO_USERNS"); no != nullptr && no[0] == '1') {
        return false;
    }
    Fixture fx;
    const Path mnt = fx.dir("probe-mnt");
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        const uid_t uid = ::getuid();
        const gid_t gid = ::getgid();
        const auto put = [](const char* file, const std::string& text) {
            const int fd = ::open(file, O_WRONLY);
            if (fd < 0) {
                return false;
            }
            const bool ok =
                ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
            ::close(fd);
            return ok;
        };
        const bool ok =
            ::unshare(CLONE_NEWUSER | CLONE_NEWNS) == 0 && put("/proc/self/setgroups", "deny") &&
            put("/proc/self/uid_map", std::to_string(uid) + " " + std::to_string(uid) + " 1") &&
            put("/proc/self/gid_map", std::to_string(gid) + " " + std::to_string(gid) + " 1") &&
            ::mount("tmpfs", mnt.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "size=1m") == 0;
        ::_exit(ok ? 0 : 77);
    }
    int status = 0;
    return ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

TEST_CASE(
    "Cancel storm, copy: no .part, no truncated file, source intact, completed items undoable",
    "[ops][fault][cancel][storm]") {
    for (int seed = 1; seed <= kSeeds; ++seed) {
        const int mode = seed % 3;
        CAPTURE(seed, mode);
        Fixture fx;
        Rig rig(fx);
        const std::vector<Path> tops = build_mixed(fx, "src", static_cast<unsigned>(seed));
        const std::string src_digest = tree_digest(fx / "src");
        const std::map<Path, std::string> top_digest = digests_of(tops);
        const Path dst = fx.dir("dst");
        Storm storm;
        arm_storm(rig, storm, static_cast<unsigned>(seed), mode);
        auto r = run(rig, copy_req(tops, dst), {}, storm.stop.get_token());
        if (storm.timer.joinable()) {
            storm.timer.join();
        }
        REQUIRE(r);
        CHECK(part_files(fx.root()).empty());
        CHECK(tree_digest(fx / "src") == src_digest); // a copy never changes its source
        check_no_truncated(fx / "src", dst);
        std::vector<Path> done_dst;
        for (const ops::ItemOutcome& o : r->items) {
            if (o.status == ItemStatus::Done) {
                INFO("done item " << o.source.native());
                CHECK_FALSE(o.partial);
                CHECK(item_digest(o.destination) == top_digest.at(o.source));
                done_dst.push_back(o.destination);
            }
        }
        if (r->record) {
            std::string dbg;
            for (const ops::ItemOutcome& o : r->items) {
                dbg += o.source.filename().native() + ":" +
                       std::to_string(static_cast<int>(o.status)) + (o.partial ? "p" : "") + " ";
            }
            INFO("outcomes " << dbg << " steps " << rec(*r).steps.size() << " done "
                             << done_dst.size());
            CHECK(rec(*r).steps.size() >= done_dst.size());
            auto u = run(rig, undo_req(rec(*r)));
            REQUIRE(u);
            for (const Path& d : done_dst) {
                CHECK_FALSE(present(d)); // went to the trash
            }
            for (const ops::UndoStep& st : rec(*r).steps) {
                CHECK_FALSE(present(st.after.path)); // every recorded step was undone
            }
            CHECK(tree_digest(fx / "src") == src_digest);
            CHECK(part_files(fx.root()).empty());
            // The trashed copies are complete (nothing was unlinked behind our back).
            for (const std::string& n : rig.trash_names()) {
                const Path in_trash = rig.home_trash / "files" / n;
                const auto want = std::find_if(done_dst.begin(), done_dst.end(),
                                               [&](const Path& d) { return d.filename() == n; });
                if (want != done_dst.end()) {
                    // the trash entry has the same digest as the original top-level source
                    for (const auto& [s, dg] : top_digest) {
                        if (s.filename() == n) {
                            CHECK(item_digest(in_trash) == dg);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("Cancel storm, same-filesystem move: every item whole at the source or the destination",
          "[ops][fault][cancel][storm][move]") {
    for (int seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        Fixture fx;
        Rig rig(fx);
        const std::vector<Path> tops = build_mixed(fx, "src", static_cast<unsigned>(seed));
        const std::map<Path, std::string> top_digest = digests_of(tops);
        const Path dst = fx.dir("dst");
        Storm storm;
        arm_storm(rig, storm, static_cast<unsigned>(seed), seed % 3);
        auto r = run(rig, move_req(tops, dst), {}, storm.stop.get_token());
        if (storm.timer.joinable()) {
            storm.timer.join();
        }
        REQUIRE(r);
        CHECK(part_files(fx.root()).empty());
        for (const auto& [s, dg] : top_digest) {
            INFO(s.filename().native());
            const Path moved = dst / s.filename();
            CHECK(present(s) != present(moved)); // exactly one place
            CHECK(item_digest(present(s) ? s : moved) == dg);
        }
        if (r->record) {
            auto u = run(rig, undo_req(rec(*r)));
            REQUIRE(u);
            for (const auto& [s, dg] : top_digest) {
                CHECK(item_digest(s) == dg); // everything is home again after the undo
            }
            CHECK(count_entries(dst) == 0);
        }
    }
}

TEST_CASE("Cancel storm, cross-filesystem move (EXDEV seam): source loss is impossible",
          "[ops][fault][cancel][storm][xfs]") {
    for (int seed = 1; seed <= kSeeds; ++seed) {
        const int mode = seed % 3;
        CAPTURE(seed, mode);
        Fixture fx;
        Rig rig(fx);
        const std::vector<Path> tops = build_mixed(fx, "src", static_cast<unsigned>(seed));
        const std::map<Path, std::string> top_digest = digests_of(tops);
        const Path dst = fx.dir("dst");
        Storm storm;
        arm_storm(rig, storm, static_cast<unsigned>(seed), mode);
        make_xdev(rig);
        auto r = run(rig, move_req(tops, dst), {}, storm.stop.get_token());
        if (storm.timer.joinable()) {
            storm.timer.join();
        }
        REQUIRE(r);
        CHECK(part_files(fx.root()).empty());
        std::size_t whole_at_dst = 0;
        for (const ops::ItemOutcome& o : r->items) {
            const Path s = o.source;
            const Path moved = dst / s.filename();
            INFO(s.filename().native() << " status " << static_cast<int>(o.status));
            if (o.status == ItemStatus::Done) {
                ++whole_at_dst;
                CHECK_FALSE(present(s));
                CHECK(item_digest(moved) == top_digest.at(s));
            } else if (o.status == ItemStatus::Cancelled || o.status == ItemStatus::Failed) {
                // Not completed: the source is intact. A destination may exist only when the
                // source was verified... never partially: if it is there it equals the source.
                CHECK(item_digest(s) == top_digest.at(s));
                if (present(moved)) {
                    CHECK(item_digest(moved) == top_digest.at(s)); // S9: no half-copied folder
                }
            }
        }
        if (r->record) {
            CHECK(rec(*r).steps.size() == whole_at_dst);
            auto u = run(rig, undo_req(rec(*r)));
            REQUIRE(u);
            for (const auto& [s, dg] : top_digest) {
                CHECK(item_digest(s) == dg);
            }
        }
    }
}

TEST_CASE("Real cross-filesystem move to /dev/shm, source tree checksum compared, undo back",
          "[ops][fault][xfs]") {
    Fixture fx;
    Fixture shm(Fixture::Where::Shm);
    if (!Fixture::shm_is_other_filesystem(fx.root())) {
        SKIP("/dev/shm is not a separate writable filesystem here");
    }
    Rig rig(fx);
    const std::vector<Path> tops = build_mixed(fx, "src", 99);
    const std::map<Path, std::string> top_digest = digests_of(tops);
    const Path dst = shm.dir("dst");
    auto r = run(rig, move_req(tops, dst));
    REQUIRE(r);
    CHECK(r->failed == 0);
    CHECK(r->done == tops.size());
    for (const auto& [s, dg] : top_digest) {
        CHECK_FALSE(present(s));
        CHECK(item_digest(dst / s.filename()) == dg);
    }
    // Hard links survived on the other filesystem.
    CHECK(lstat_of(dst / "t2/h1").st_ino == lstat_of(dst / "t2/h2").st_ino);
    CHECK(S_ISFIFO(lstat_of(dst / "t2/pipe").st_mode));
    REQUIRE(r->record.has_value());
    auto u = run(rig, undo_req(rec(*r)));
    REQUIRE(u);
    CHECK(u->failed == 0);
    for (const auto& [s, dg] : top_digest) {
        CHECK(item_digest(s) == dg);
    }
    CHECK(part_files(fx.root()).empty());
    CHECK(part_files(shm.root()).empty());
}

TEST_CASE("Injected ENOSPC, EIO and EACCES leave no temp file and no truncated destination",
          "[ops][fault][inject]") {
    struct Case {
        const char* name;
        IoSite site;
        int err;
        ErrorCode code;
        bool sync;
    };
    const Case cases[] = {
        {"ENOSPC at write", IoSite::Write, ENOSPC, ErrorCode::NoSpace, false},
        {"EIO at write", IoSite::Write, EIO, ErrorCode::IoError, false},
        {"EIO at fsync", IoSite::Fsync, EIO, ErrorCode::IoError, true},
        {"EACCES at create", IoSite::CreateTemp, EACCES, ErrorCode::PermissionDenied, false},
        {"ENOSPC at commit", IoSite::Commit, ENOSPC, ErrorCode::NoSpace, false},
        {"EIO at mkdir", IoSite::Mkdir, EIO, ErrorCode::IoError, false}};
    for (const Case& c : cases) {
        for (int nth = 1; nth <= 6; ++nth) {
            CAPTURE(c.name, nth);
            Fixture fx;
            Rig rig(fx);
            const std::vector<Path> tops = build_mixed(fx, "src", 5);
            const std::string src_digest = tree_digest(fx / "src");
            const Path dst = fx.dir("dst");
            std::atomic<int> seen{0};
            const int nth_copy = nth;
            std::string hit; // where the error was injected (single worker thread here)
            std::set<Path> poisoned;
            // A real disk error does not go away on the engine's retry (copy_file_range ->
            // read/write fallback): once triggered, the same path keeps failing at that site.
            // Directory fsyncs are best effort by design and are not counted.
            rig.user_io = [&, nth_copy](IoSite site, const Path& p, std::uint64_t off) {
                if (site != c.site) {
                    return 0;
                }
                if (poisoned.contains(p)) {
                    return c.err;
                }
                if (site == IoSite::Fsync && !S_ISREG(lstat_of(p).st_mode)) {
                    return 0;
                }
                if (++seen == nth_copy) {
                    hit = p.filename().native() + " @" + std::to_string(off);
                    poisoned.insert(p);
                    return c.err;
                }
                return 0;
            };
            ops::OpRequest req = copy_req(tops, dst);
            req.options.sync_copies = c.sync;
            auto r = run(rig, req);
            REQUIRE(r);
            if (seen < nth) {
                continue; // fewer sites than nth: nothing was injected
            }
            // A failing entry inside a folder leaves that folder's item Done with `partial` set
            // (ops.hpp ItemOutcome::partial); a failing top-level item is Failed.
            bool found = false;
            std::string dbg;
            for (const ops::ItemOutcome& o : r->items) {
                dbg += o.source.filename().native() + ":" +
                       std::to_string(static_cast<int>(o.status)) + (o.partial ? "p" : "") +
                       (o.error ? "e" : "") + " ";
            }
            INFO("outcomes " << dbg << " failed " << r->failed << " injected sites " << seen.load()
                             << " hit " << hit);
            for (const ops::ItemOutcome& o : r->items) {
                if (o.status == ItemStatus::Failed) {
                    found = true;
                    CHECK(code_of(o) == c.code);
                } else if (o.partial) {
                    found = true;
                    CHECK(code_of(o) == c.code);
                }
            }
            CHECK(found);
            CHECK(part_files(fx.root()).empty());
            CHECK(tree_digest(fx / "src") == src_digest);
            check_no_truncated(fx / "src", dst);
            // Items that did complete are whole and undoable; the failed ones are not recorded.
            for (const ops::ItemOutcome& o : r->items) {
                if (o.status == ItemStatus::Done && !o.partial) {
                    CHECK(item_digest(o.destination) == item_digest(o.source));
                }
            }
        }
    }
}

TEST_CASE("NoSpace is decided before anything is written (S14)", "[ops][fault][space]") {
    Fixture fx;
    Rig rig(fx);
    const std::vector<Path> tops = build_mixed(fx, "src", 3);
    const Path dst = fx.dir("dst");
    ops::detail::set_free_space_hook(
        [](const Path&) -> std::optional<std::uint64_t> { return 1000; });
    auto r = run(rig, copy_req(tops, dst));
    ops::detail::set_free_space_hook({});
    if (r) {
        CHECK(r->done == 0);
    } else {
        CHECK(r.error().code == ErrorCode::NoSpace);
    }
    CHECK(count_entries(dst) == 0); // nothing written, not even a temp file
}

TEST_CASE("A real read-only destination: PermissionDenied, nothing created, source intact",
          "[ops][fault][permission]") {
    if (::getuid() == 0) {
        SKIP("root ignores directory permissions");
    }
    Fixture fx;
    Rig rig(fx);
    fx.file("src/a.txt", "A");
    fx.file("src/tree/b.txt", "B");
    const Path dst = fx.dir("dst");
    ::chmod(dst.c_str(), 0555);
    auto r = run(rig, copy_req({fx / "src/a.txt", fx / "src/tree"}, dst));
    REQUIRE(r);
    CHECK(r->done == 0);
    CHECK(r->failed == 2);
    for (const ops::ItemOutcome& o : r->items) {
        CHECK(code_of(o) == ErrorCode::PermissionDenied);
    }
    ::chmod(dst.c_str(), 0755);
    CHECK(count_entries(dst) == 0);
    CHECK(read_file(fx / "src/a.txt") == "A");
    // A read-only folder in the middle of a tree: the rest is copied, the failure is reported.
    fx.file("src2/ok.txt", "ok");
    fx.file("src2/locked/x.txt", "x");
    const Path dst2 = fx.dir("dst2");
    fx.dir("dst2/src2/locked");
    ::chmod((fx / "dst2/src2/locked").c_str(), 0555);
    auto r2 = run(rig, copy_req({fx / "src2"}, dst2), always(ConflictAction::Merge));
    REQUIRE(r2);
    CHECK(part_files(fx.root()).empty());
    CHECK(read_file(fx / "src2/locked/x.txt") == "x");
    ::chmod((fx / "dst2/src2/locked").c_str(), 0755);
}

TEST_CASE(
    "Real ENOSPC on a 1 MiB tmpfs (private user+mount namespace): no temp file, no truncation",
    "[ops][fault][enospc]") {
    if (kTsan) {
        // unshare(CLONE_NEWUSER) needs a single-threaded process and TSan's runtime has threads;
        // the same case runs in the plain and ASan builds.
        SUCCEED("not run under ThreadSanitizer");
        return;
    }
    // Probe FIRST, before any assertion: a SKIP after a passed assertion is reported by ctest as
    // Failed (Catch2's exit code is then not the skip code).
    if (!userns_tmpfs_usable()) {
        SKIP("cannot create a user+mount namespace here (restricted unprivileged user namespaces)");
    }
    Fixture fx;
    const Path mnt = fx.dir("mnt");
    fx.file("src/one.bin", std::string(std::size_t{4} * 1024U * 1024U, 'q'));
    std::vector<Path> many;
    many.reserve(300);
    for (int i = 0; i < 300; ++i) {
        many.push_back(fx.file("src/many/f" + std::to_string(i),
                               std::string(8192, static_cast<char>('a' + i % 26))));
    }
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(fds[0]);
        const auto report = [&](int code, const std::string& msg) {
            const std::string m = msg + "\n";
            (void)!::write(fds[1], m.data(), m.size());
            ::_exit(code);
        };
        const uid_t uid = ::getuid();
        const gid_t gid = ::getgid();
        if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
            report(77, "unshare failed");
        }
        const auto put = [](const char* file, const std::string& text) {
            const int fd = ::open(file, O_WRONLY);
            if (fd < 0) {
                return false;
            }
            const bool ok =
                ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
            ::close(fd);
            return ok;
        };
        if (!put("/proc/self/setgroups", "deny") ||
            !put("/proc/self/uid_map", std::to_string(uid) + " " + std::to_string(uid) + " 1") ||
            !put("/proc/self/gid_map", std::to_string(gid) + " " + std::to_string(gid) + " 1")) {
            report(77, "uid_map failed");
        }
        if (::mount("tmpfs", mnt.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "size=1m") != 0) {
            report(77, "mount failed");
        }
        // Everything below runs inside the namespace; paths are fixture paths.
        Rig rig(fx);
        struct statvfs before {};
        ::statvfs(mnt.c_str(), &before);
        // Claim lots of space so the engine starts writing and meets the REAL ENOSPC.
        ops::detail::set_free_space_hook(
            [](const Path&) -> std::optional<std::uint64_t> { return std::uint64_t{1} << 40; });
        auto r = run(rig, copy_req({fx / "src/one.bin"}, mnt));
        if (!r || r->failed != 1 || code_of(r->items[0]) != ErrorCode::NoSpace) {
            report(1, "big file: expected one NoSpace failure");
        }
        if (!part_files(mnt).empty() || count_entries(mnt) != 0) {
            report(2, "big file: temp or truncated file left on the tmpfs");
        }
        auto r2 = run(rig, copy_req({fx / "src/many"}, mnt));
        if (!r2 || r2->failed + r2->done < 1) {
            report(3, "tree: no outcome");
        }
        if (!part_files(mnt).empty()) {
            report(4, "tree: temp file left");
        }
        std::size_t good = 0;
        for (auto it = stdfs::recursive_directory_iterator(mnt);
             it != stdfs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file()) {
                const Path rel = it->path().lexically_relative(mnt);
                if (hash_file(it->path()) != hash_file(fx / "src" / rel)) {
                    report(5, "tree: truncated file " + rel.native());
                }
                ++good;
            }
        }
        if (good == 0 || good >= 300) {
            report(6, "tree: expected some but not all files, got " + std::to_string(good));
        }
        bool saw_nospace = false;
        for (const ops::ItemOutcome& o : r2->items) {
            saw_nospace = saw_nospace || code_of(o) == ErrorCode::NoSpace;
        }
        if (!saw_nospace) {
            report(7, "tree: no NoSpace outcome");
        }
        // A full volume lets the source stay intact and the next copy elsewhere work.
        ops::detail::set_free_space_hook({});
        report(0, "ok " + std::to_string(good) + " of 300 files fitted");
    }
    ::close(fds[1]);
    char buf[256] = {};
    const ssize_t n = ::read(fds[0], buf, sizeof buf - 1);
    ::close(fds[0]);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    const std::string message(buf, n > 0 ? static_cast<std::size_t>(n) : 0U);
    INFO(message);
    REQUIRE(WIFEXITED(status));
    if (WEXITSTATUS(status) == 77) {
        SKIP("cannot create a user+mount namespace here: " << message);
    }
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(count_entries(mnt) == 0); // the tmpfs lived only in the child's namespace
}

TEST_CASE("Symlink traps: no operation, undo or redo ever touches the sentinel tree",
          "[ops][fault][symlink]") {
    Fixture fx;
    Fixture outside; // the sentinel lives in another fixture
    outside.file("secret/key.txt", "do not touch");
    outside.file("secret/deep/more.bin", std::string(100000, 's'));
    outside.file("lone.txt", "lone sentinel");
    const std::string sentinel = tree_digest(outside.root());
    const std::uint64_t secret_hash = hash_file(outside / "secret/key.txt");
    std::optional<Forbidden> forbid(std::in_place,
                                    outside.root()); // any engine path inside it aborts
    Rig rig(fx);

    const auto build = [&] {
        fx.file("trap/real.txt", "real");
        fx.dir("trap/inner");
        fx.link("trap/to-dir", (outside / "secret").native());
        fx.link("trap/to-file", (outside / "lone.txt").native());
        fx.link("trap/inner/up", "../..");      // loops back above the tree
        fx.link("trap/inner/self", "../inner"); // loop onto itself
        fx.link("trap/rel-out", "../../../../.." + (outside / "secret").native());
        fx.link("trap/dangling", "nowhere");
    };
    build();
    const Path trap = fx / "trap";
    const std::string trap_digest = tree_digest(trap);

    const auto sentinel_ok = [&] {
        CHECK(tree_digest(outside.root()) == sentinel);
        CHECK(hash_file(outside / "secret/key.txt") == secret_hash);
    };

    SECTION("copy: links are copied as links; undo and redo too") {
        const Path dst = fx.dir("dst");
        auto c = run(rig, copy_req({trap}, dst));
        REQUIRE(c);
        CHECK(c->failed == 0);
        CHECK(tree_digest(dst / "trap") == trap_digest); // identical links, none followed
        auto u = run(rig, undo_req(rec(*c)));
        REQUIRE(u);
        auto rd = run(rig, redo_req(rec(*u)));
        REQUIRE(rd);
        sentinel_ok();
        CHECK(tree_digest(trap) == trap_digest);
    }
    SECTION("copy with dereference only of a top-level link targets exactly that link") {
        const Path dst = fx.dir("dst");
        ops::OpRequest req = copy_req({trap / "to-file"}, dst);
        req.options.dereference_top_level = true;
        // This one is allowed to READ the target (dereference_top_level), so the forbid guard is
        // lifted here: the engine names the link, then opens its target. The result is a
        // regular file with the sentinel's bytes, the sentinel itself unchanged.
        forbid.reset();
        auto c = run(rig, req);
        REQUIRE(c);
        CHECK(read_file(dst / "to-file") == "lone sentinel");
        CHECK_FALSE(S_ISLNK(lstat_of(dst / "to-file").st_mode));
        sentinel_ok();
    }
    SECTION("move (same filesystem and EXDEV), undo") {
        const Path dst = fx.dir("dst");
        auto m = run(rig, move_req({trap}, dst));
        REQUIRE(m);
        CHECK(tree_digest(dst / "trap") == trap_digest);
        auto u = run(rig, undo_req(rec(*m)));
        REQUIRE(u);
        CHECK(tree_digest(trap) == trap_digest);
        make_xdev(rig);
        auto mx = run(rig, move_req({trap}, dst));
        REQUIRE(mx);
        CHECK(mx->failed == 0);
        CHECK(tree_digest(dst / "trap") == trap_digest);
        CHECK_FALSE(present(trap));
        auto ux = run(rig, undo_req(rec(*mx)));
        REQUIRE(ux);
        CHECK(tree_digest(trap) == trap_digest);
        sentinel_ok();
    }
    SECTION("trash, restore, undo of trash") {
        auto t = run(rig, trash_req({trap / "to-dir", trap / "to-file"}));
        REQUIRE(t);
        CHECK(t->failed == 0);
        CHECK(list_trash(rig).size() == 2); // the links were trashed, not their targets
        sentinel_ok();
        auto u = run(rig, undo_req(rec(*t)));
        REQUIRE(u);
        CHECK(tree_digest(trap) == trap_digest);
        auto whole = run(rig, trash_req({trap}));
        REQUIRE(whole);
        auto back = run(rig, restore_req(list_trash(rig)));
        REQUIRE(back);
        CHECK(tree_digest(trap) == trap_digest);
        sentinel_ok();
    }
    SECTION("confirmed permanent delete removes the links, never their targets") {
        auto d = run(rig, delete_req({trap}));
        REQUIRE(d);
        CHECK(d->failed == 0);
        CHECK_FALSE(present(trap));
        sentinel_ok();
        build();
        auto d2 = run(rig, delete_req({trap / "to-dir", trap / "to-file"}));
        REQUIRE(d2);
        CHECK_FALSE(present(trap / "to-dir"));
        CHECK_FALSE(present(trap / "to-file"));
        sentinel_ok();
    }
    SECTION("a folder swapped for a symlink to the sentinel while an operation runs") {
        fx.file("victim/vdir/v.txt", "v");
        fx.file("victim/other.txt", "o");
        const Path victim = fx / "victim";
        std::atomic<bool> swapped{false};
        rig.user_inject = [&](const Path& p) {
            if (p == victim / "vdir" && !swapped.exchange(true)) {
                // The classic race: replace the folder by a link to the sentinel after it was
                // scanned. The engine's openat(O_NOFOLLOW) must refuse to enter it.
                std::error_code ec;
                stdfs::rename(victim / "vdir", victim / "vdir.moved", ec);
                (void)!::symlink((outside / "secret").c_str(), (victim / "vdir").c_str());
            }
            return true;
        };
        auto d = run(rig, delete_req({victim}));
        REQUIRE(d);
        sentinel_ok();
        stdfs::remove(victim / "vdir"); // the link we planted (never the target)
        sentinel_ok();
    }
}

TEST_CASE("Hard links, FIFOs and sockets: links kept, FIFOs recreated without opening, sockets "
          "refused",
          "[ops][fault][special]") {
    Fixture fx;
    Rig rig(fx);
    const Path h1 = fx.file("s/h1", "shared inode");
    REQUIRE(::link(h1.c_str(), (fx / "s/h2").c_str()) == 0);
    REQUIRE(::link(h1.c_str(), (fx / "s/sub-h3").c_str()) == 0);
    fx.fifo("s/pipe");
    // A socket: bind through /proc/self/fd so the long fixture path does not overflow sun_path.
    const int dirfd = ::open((fx / "s").c_str(), O_RDONLY | O_DIRECTORY);
    REQUIRE(dirfd >= 0);
    const int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(sock >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string via = "/proc/self/fd/" + std::to_string(dirfd) + "/sock";
    std::strncpy(addr.sun_path, via.c_str(), sizeof addr.sun_path - 1);
    REQUIRE(::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    ::close(dirfd);
    REQUIRE(S_ISSOCK(lstat_of(fx / "s/sock").st_mode));
    const Path dst = fx.dir("dst");

    // The engine must never open the FIFO (that would block forever): a watchdog aborts then.
    std::promise<Result<ops::OpResult>> done;
    auto fut = done.get_future();
    std::thread worker([&] { done.set_value(run(rig, copy_req({fx / "s"}, dst))); });
    if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        die("copy of a tree with a FIFO did not finish in 30 s (the engine opened the FIFO?)");
    }
    worker.join();
    Result<ops::OpResult> r = fut.get();
    REQUIRE(r);
    CHECK(S_ISFIFO(lstat_of(dst / "s/pipe").st_mode));
    CHECK_FALSE(present(dst / "s/sock")); // sockets are not copied
    CHECK(r->failed + r->done >= 1);
    const struct stat a = lstat_of(dst / "s/h1");
    const struct stat b = lstat_of(dst / "s/h2");
    const struct stat c = lstat_of(dst / "s/sub-h3");
    CHECK(a.st_ino == b.st_ino);
    CHECK(a.st_ino == c.st_ino);
    CHECK(a.st_nlink == 3);
    CHECK(a.st_ino != lstat_of(h1).st_ino); // the copy is separate from the original
    CHECK(read_file(dst / "s/h2") == "shared inode");
    CHECK(part_files(fx.root()).empty());

    // A socket and a FIFO can be trashed and restored, and moved within a filesystem.
    auto t = run(rig, trash_req({fx / "s/sock", fx / "s/pipe"}));
    REQUIRE(t);
    CHECK(t->failed == 0);
    auto items = list_trash(rig);
    auto back = run(rig, restore_req(items));
    REQUIRE(back);
    CHECK(S_ISSOCK(lstat_of(fx / "s/sock").st_mode));
    CHECK(S_ISFIFO(lstat_of(fx / "s/pipe").st_mode));
    ::close(sock);
}

TEST_CASE("A tree deeper than PATH_MAX: copy, trash, undo and permanent delete are dirfd-relative",
          "[ops][fault][deep]") {
    Fixture fx;
    Rig rig(fx);
    constexpr int kDepth = 2100; // "d/" x 2100 = 4200 bytes > PATH_MAX (4096)
    const Path top = fx.dir("deep");
    const auto build_chain = [&](const Path& root, int depth) {
        int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY);
        REQUIRE(fd >= 0);
        for (int i = 0; i < depth; ++i) {
            REQUIRE(::mkdirat(fd, "d", 0755) == 0);
            const int next = ::openat(fd, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            REQUIRE(next >= 0);
            if (i % 500 == 0) {
                const int f = ::openat(fd, "mid.txt", O_WRONLY | O_CREAT, 0644);
                REQUIRE(f >= 0);
                (void)!::write(f, "mid", 3);
                ::close(f);
            }
            ::close(fd);
            fd = next;
        }
        const int f = ::openat(fd, "leaf.txt", O_WRONLY | O_CREAT, 0644);
        REQUIRE(f >= 0);
        REQUIRE(::write(f, "leaf!", 5) == 5);
        ::close(f);
        ::close(fd);
    };
    // Depth and leaf of a chain, walked one level at a time (never holds two fds).
    const auto chain = [](const Path& root) -> std::pair<int, std::string> {
        int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY);
        int depth = 0;
        while (fd >= 0) {
            const int next = ::openat(fd, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (next < 0) {
                break;
            }
            ::close(fd);
            fd = next;
            ++depth;
        }
        std::string leaf;
        if (fd >= 0) {
            const int f = ::openat(fd, "leaf.txt", O_RDONLY | O_NOFOLLOW);
            if (f >= 0) {
                char buf[16] = {};
                const ssize_t n = ::read(f, buf, sizeof buf);
                leaf.assign(buf, n > 0 ? static_cast<std::size_t>(n) : 0U);
                ::close(f);
            }
            ::close(fd);
        }
        return {depth, leaf};
    };
    // Bottom-up removal that does not need a long path: O(n^2) openats, bounded and cheap.
    const auto remove_chain = [&](const Path& root) {
        for (int depth = kDepth + 5; depth >= 0; --depth) {
            int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY);
            for (int i = 0; i < depth && fd >= 0; ++i) {
                const int next = ::openat(fd, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                ::close(fd);
                fd = next;
            }
            if (fd < 0) {
                continue;
            }
            ::unlinkat(fd, "leaf.txt", 0);
            ::unlinkat(fd, "mid.txt", 0);
            ::unlinkat(fd, "d", AT_REMOVEDIR);
            ::close(fd);
        }
    };
    build_chain(top, kDepth);
    REQUIRE(chain(top) == std::pair<int, std::string>{kDepth, "leaf!"});
    const Path dst = fx.dir("dst");

    auto c = run(rig, copy_req({top}, dst));
    if (!c || c->failed != 0) {
        remove_chain(top);
        remove_chain(dst / "deep");
    }
    REQUIRE(c);
    CHECK(c->failed == 0);
    CHECK(chain(dst / "deep") == std::pair<int, std::string>{kDepth, "leaf!"});
    CHECK(chain(top) == std::pair<int, std::string>{kDepth, "leaf!"});

    auto u = run(rig, undo_req(rec(*c))); // the copy goes to the trash (one rename)
    REQUIRE(u);
    CHECK_FALSE(present(dst / "deep"));
    CHECK(chain(rig.home_trash / "files/deep") == std::pair<int, std::string>{kDepth, "leaf!"});

    // Permanent delete of the trashed copy and of the original, both deeper than PATH_MAX.
    auto items = list_trash(rig);
    REQUIRE(items.size() == 1);
    auto d1 = run(rig, delete_trashed_req(items));
    REQUIRE(d1);
    CHECK(d1->failed == 0);
    CHECK_FALSE(present(rig.home_trash / "files/deep"));
    auto d2 = run(rig, delete_req({top}));
    if (!d2 || d2->failed != 0) {
        remove_chain(top);
    }
    REQUIRE(d2);
    CHECK(d2->failed == 0);
    CHECK_FALSE(present(top));
    remove_chain(top); // no-op safety net
}

TEST_CASE("Protected sources (fake home, mount point, trash) are refused; copying them is allowed",
          "[ops][fault][protected]") {
    Fixture fx;
    Rig rig(fx);
    const Path home = fx.dir("home");
    fx.file("home/Belgeler/rapor.txt", "r");
    const Path mnt = fx.dir("mnt/usb");
    fx.file("mnt/usb/file.txt", "u");
    rig.engine.protected_paths.home = home;
    rig.engine.protected_paths.mount_points = {mnt};
    // The trash directory exists for real after one trash operation.
    const Path victim = fx.file("loose.txt", "l");
    REQUIRE(run(rig, trash_req({victim})));
    rig.engine.protected_paths.trash_roots = {rig.home_trash};
    const Path dst = fx.dir("dst");
    const std::string everything = tree_digest(fx.root());

    const std::vector<Path> protected_items{
        home,           home.parent_path(),       mnt,
        rig.home_trash, rig.home_trash / "files", rig.home_trash / "files/loose.txt"};
    const OpKind kinds[] = {OpKind::Move, OpKind::Trash, OpKind::DeletePermanently, OpKind::Rename};
    for (const Path& p : protected_items) {
        for (const OpKind kind : kinds) {
            INFO(p.native() << " kind " << ops::to_string(kind));
            ops::OpRequest req;
            req.kind = kind;
            req.sources = {p};
            req.destination = dst;
            req.new_name = "renamed";
            req.confirmed = true; // even a confirmed delete is refused for a protected path
            auto r = run(rig, req);
            CHECK_FALSE(r);
            if (!r) {
                CHECK((r.error().code == ErrorCode::PermissionDenied ||
                       r.error().code == ErrorCode::InvalidArgument));
            }
        }
    }
    CHECK(tree_digest(fx.root()) == everything); // nothing moved, trashed, renamed or deleted
    // The same through the queue: submit() rejects them (non-blocking, lexical).
    ops::QueueOptions qo;
    qo.engine = rig.engine;
    auto queue = ops::create_operation_queue(qo);
    REQUIRE(queue);
    for (const Path& p : protected_items) {
        auto id = (*queue)->submit(trash_req({p}));
        CHECK_FALSE(id);
        auto id2 = (*queue)->submit(delete_req({p}));
        CHECK_FALSE(id2);
    }
    // Copying is allowed.
    auto c = run(rig, copy_req({home, mnt}, dst));
    REQUIRE(c);
    CHECK(c->failed == 0);
    CHECK(read_file(dst / "home/Belgeler/rapor.txt") == "r");
    CHECK(read_file(dst / "usb/file.txt") == "u");
    (*queue)->shutdown();
}

TEST_CASE("Copying a folder into itself, also through a symlink, is refused or harmless",
          "[ops][fault][self]") {
    Fixture fx;
    Rig rig(fx);
    fx.file("a/f.txt", "f");
    fx.file("a/sub/g.txt", "g");
    fx.link("lnk", (fx / "a/sub").native());
    fx.link("up", (fx / "a").native());
    const std::string before = tree_digest(fx / "a");

    // Lexically inside: refused outright.
    CHECK_FALSE(run(rig, copy_req({fx / "a"}, fx / "a/sub")));
    CHECK_FALSE(run(rig, move_req({fx / "a"}, fx / "a/sub")));
    // Through a link: lexically outside, really inside. S15: refused at execution.
    for (const Path& via : {fx / "lnk", fx / "up", fx / "up/sub"}) {
        for (const OpKind kind : {OpKind::Copy, OpKind::Move}) {
            INFO(via.native() << " " << ops::to_string(kind));
            ops::OpRequest req = copy_req({fx / "a"}, via);
            req.kind = kind;
            std::promise<Result<ops::OpResult>> done;
            auto fut = done.get_future();
            std::thread worker(
                [&] { done.set_value(run(rig, req, always(ConflictAction::KeepBoth))); });
            if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
                die("copy of a folder into itself through a link did not terminate");
            }
            worker.join();
            Result<ops::OpResult> r = fut.get();
            if (r) {
                // Accepted as a request: then no item may have succeeded into itself.
                for (const ops::ItemOutcome& o : r->items) {
                    CHECK(o.status != ItemStatus::Done);
                }
            }
            CHECK(tree_digest(fx / "a") == before); // never grew a copy of itself
            CHECK(part_files(fx.root()).empty());
        }
    }
}

TEST_CASE("100k small files: copy, undo, redo, trash and delete finish in sane time",
          "[ops][fault][big]") {
    const int dirs = kSanitized ? 20 : 100;
    const int per_dir = 1000;
    const std::size_t total = static_cast<std::size_t>(dirs) * per_dir;
    Fixture fx;
    Rig rig(fx);
    const Path src = fx.dir("big");
    for (int d = 0; d < dirs; ++d) {
        const Path dir = src / ("d" + std::to_string(d));
        stdfs::create_directory(dir);
        for (int i = 0; i < per_dir; ++i) {
            Fixture::write_raw(dir / ("f" + std::to_string(i) + ".txt"), "0123456789");
        }
    }
    const std::string digest = tree_digest(src);
    const Path dst = fx.dir("dst");
    using Clock = std::chrono::steady_clock;
    const auto secs = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };

    const auto t0 = Clock::now();
    auto c = run(rig, copy_req({src}, dst));
    const auto t1 = Clock::now();
    REQUIRE(c);
    CHECK(c->failed == 0);
    CHECK(count_entries(dst / "big") == total + static_cast<std::size_t>(dirs));
    CHECK(tree_digest(dst / "big") == digest);
    INFO("copy of " << total << " files: " << secs(t0, t1) << " s");
    CHECK(secs(t0, t1) < 300.0);

    const auto t2 = Clock::now();
    auto u = run(rig, undo_req(rec(*c))); // the copy goes to the trash: one rename
    const auto t3 = Clock::now();
    REQUIRE(u);
    CHECK_FALSE(present(dst / "big"));
    INFO("undo (trash) took " << secs(t2, t3) << " s");
    CHECK(secs(t2, t3) < 30.0);
    auto rd = run(rig, restore_req(list_trash(rig))); // and back out of the trash (Trash view)
    REQUIRE(rd);
    CHECK(tree_digest(dst / "big") == digest);

    const auto t4 = Clock::now();
    auto t = run(rig, trash_req({dst / "big"}));
    const auto t5 = Clock::now();
    REQUIRE(t);
    INFO("trash took " << secs(t4, t5) << " s");
    CHECK(secs(t4, t5) < 30.0);

    const auto items = list_trash(rig);
    REQUIRE(items.size() == 1);
    const auto t6 = Clock::now();
    auto del = run(rig, delete_trashed_req(items));
    const auto t7 = Clock::now();
    REQUIRE(del);
    CHECK(del->failed == 0);
    INFO("permanent delete took " << secs(t6, t7) << " s");
    CHECK(secs(t6, t7) < 240.0);
    CHECK(tree_digest(src) == digest); // the original was never touched
    CHECK(rig.trash_names().empty());
    CHECK(part_files(fx.root()).empty());
}

TEST_CASE("Overlapping queue operations serialize and end as a sequential run would (TSan target)",
          "[ops][queue][race]") {
    const auto setup = [](Fixture& fx) {
        for (int i = 0; i < 40; ++i) {
            fx.file("A/f" + std::to_string(i) + ".bin",
                    std::string(20000U + 997U * static_cast<unsigned>(i), 'a'));
            fx.file("A/sub/g" + std::to_string(i) + ".bin", std::string(5000, 'b'));
        }
        fx.dir("B");
        for (int i = 0; i < 40; ++i) {
            fx.file("C/c" + std::to_string(i) + ".bin", std::string(30000, 'c'));
        }
        fx.dir("E");
    };
    // The sequential twin: the same four requests, one after another, on a second fixture.
    const auto requests = [](Fixture& fx) {
        std::vector<ops::OpRequest> r;
        r.push_back(copy_req({fx / "A"}, fx / "B"));   // 1 reads A, writes B
        r.push_back(move_req({fx / "B/A"}, fx / "D")); // 2 overlaps 1 (B)
        r.push_back(trash_req({fx / "A"}));            // 3 overlaps 1 (A)
        r.push_back(copy_req({fx / "C"}, fx / "E"));   // 4 independent
        r[1].conflict = ops::ConflictPolicy::KeepBoth;
        return r;
    };
    Fixture expected_fx;
    {
        Rig rig(expected_fx);
        setup(expected_fx);
        expected_fx.dir("D");
        for (const ops::OpRequest& req : requests(expected_fx)) {
            auto r = run(rig, req);
            REQUIRE(r);
            CHECK(r->failed == 0);
        }
    }
    const std::string want_b = tree_digest(expected_fx / "B");
    const std::string want_d = tree_digest(expected_fx / "D");
    const std::string want_e = tree_digest(expected_fx / "E");
    CHECK_FALSE(present(expected_fx / "A"));

    for (int round = 0; round < 3; ++round) {
        CAPTURE(round);
        Fixture fx;
        Rig rig(fx);
        setup(fx);
        fx.dir("D");
        ops::QueueOptions qo;
        qo.engine = rig.engine;
        qo.max_concurrent = 4;
        std::atomic<int> wakes{0};
        qo.on_event = [&wakes] { ++wakes; };
        auto made = ops::create_operation_queue(qo);
        REQUIRE(made);
        ops::IOperationQueue& queue = **made;

        std::vector<ops::OpId> ids;
        for (const ops::OpRequest& req : requests(fx)) {
            guard_request(req);
            auto id = queue.submit(req);
            REQUIRE(id);
            ids.push_back(*id);
        }
        // Another thread hammers the thread-safe, non-blocking API while the workers run.
        std::atomic<bool> stop_hammer{false};
        std::thread hammer([&] {
            std::mt19937 rng(static_cast<unsigned>(round) + 1U);
            while (!stop_hammer) {
                (void)queue.snapshot();
                (void)queue.pending_conflicts();
                queue.pause(ops::OpId{999999});
                queue.resume(ops::OpId{999999});
                queue.cancel(ops::OpId{888888});
                ops::ProtectedPaths pp = rig.engine.protected_paths;
                queue.set_protected_paths(pp);
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300U));
            }
        });
        std::vector<ops::OpEvent> events;
        std::set<std::uint64_t> finished;
        const bool ok = wait_until(
            [&] {
                queue.poll(events);
                finished.clear();
                for (const ops::OpEvent& e : events) {
                    if (e.type == ops::OpEvent::Type::Finished) {
                        finished.insert(e.op.value);
                    }
                }
                return finished.size() == ids.size();
            },
            std::chrono::seconds(120));
        stop_hammer = true;
        hammer.join();
        REQUIRE(ok);

        // Order: an overlapping later operation Starts only after the earlier one Finished.
        std::map<std::uint64_t, std::size_t> started_at;
        std::map<std::uint64_t, std::size_t> finished_at;
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].type == ops::OpEvent::Type::Started) {
                started_at[events[i].op.value] = i;
            } else if (events[i].type == ops::OpEvent::Type::Finished) {
                finished_at[events[i].op.value] = i;
                REQUIRE(events[i].result != nullptr);
                CHECK(events[i].result->failed == 0);
                CHECK_FALSE(events[i].result->cancelled);
            }
        }
        CHECK(started_at.at(ids[0].value) < finished_at.at(ids[0].value));
        CHECK(finished_at.at(ids[0].value) < started_at.at(ids[1].value)); // B overlap
        CHECK(finished_at.at(ids[0].value) < started_at.at(ids[2].value)); // A overlap
        // The result is what the sequential run produced.
        CHECK(tree_digest(fx / "B") == want_b);
        CHECK(tree_digest(fx / "D") == want_d);
        CHECK(tree_digest(fx / "E") == want_e);
        CHECK_FALSE(present(fx / "A"));
        CHECK(part_files(fx.root()).empty());
        queue.shutdown();
    }
}

TEST_CASE(
    "Queue cancel storm and shutdown mid-copy: no temp file, no truncated file, no source loss",
    "[ops][queue][race][cancel]") {
    for (int seed = 1; seed <= (kSanitized ? 8 : 20); ++seed) {
        CAPTURE(seed);
        Fixture fx;
        Rig rig(fx);
        const std::vector<Path> tops = build_mixed(fx, "src", static_cast<unsigned>(seed));
        const std::string src_digest = tree_digest(fx / "src");
        const Path d1 = fx.dir("d1");
        const Path d2 = fx.dir("d2");
        ops::QueueOptions qo;
        qo.engine = rig.engine;
        qo.max_concurrent = 3;
        auto made = ops::create_operation_queue(qo);
        REQUIRE(made);
        ops::IOperationQueue& queue = **made;
        std::vector<ops::OpId> ids;
        for (const Path& dst : {d1, d2}) {
            auto a = queue.submit(copy_req(tops, dst, ops::ConflictPolicy::KeepBoth));
            REQUIRE(a);
            ids.push_back(*a);
        }
        auto m = queue.submit(copy_req({fx / "src/t1.bin"}, fx.dir("d3")));
        REQUIRE(m);
        ids.push_back(*m);
        std::mt19937 rng(std::uint_fast32_t{static_cast<unsigned>(seed)} * 31U);
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 4000U));
        const unsigned how = static_cast<unsigned>(seed) % 3U;
        if (how == 0) {
            queue.cancel_all();
        } else if (how == 1) {
            for (const ops::OpId id : ids) {
                if (rng() % 2U == 0U) {
                    queue.cancel(id);
                }
            }
            queue.pause(ids[0]);
            queue.resume(ids[0]);
            queue.cancel_all();
        } else {
            queue.shutdown(); // quit while operations run
        }
        std::vector<ops::OpEvent> events;
        std::size_t finished = 0;
        (void)wait_until(
            [&] {
                queue.poll(events);
                finished = 0;
                for (const ops::OpEvent& e : events) {
                    finished += e.type == ops::OpEvent::Type::Finished ? 1U : 0U;
                }
                return finished == ids.size();
            },
            std::chrono::seconds(60));
        CHECK(finished == ids.size());
        queue.shutdown();
        CHECK(part_files(fx.root()).empty());
        CHECK(tree_digest(fx / "src") == src_digest);
        for (const Path& dst : {d1, d2, fx / "d3"}) {
            check_no_truncated(fx / "src", dst);
        }
        for (const ops::OpEvent& e : events) {
            if (e.type == ops::OpEvent::Type::Finished && e.result) {
                for (const ops::ItemOutcome& o : e.result->items) {
                    if (o.status == ItemStatus::Done) {
                        CHECK(item_digest(o.destination) == item_digest(o.source));
                    }
                }
            }
        }
        // Whatever completed is undoable; the undo consistent: sources intact, no temp files.
        for (const ops::OpEvent& e : events) {
            if (e.type == ops::OpEvent::Type::Finished && e.result &&
                e.result->record.has_value()) {
                auto u = run(rig, undo_req(rec(*e.result)));
                REQUIRE(u);
            }
        }
        CHECK(tree_digest(fx / "src") == src_digest);
        CHECK(part_files(fx.root()).empty());
    }
}
