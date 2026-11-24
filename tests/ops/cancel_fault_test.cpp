// Cancellation (S9) at many points and injected faults (ENOSPC, EIO, EACCES) mid-copy: never a
// temp file left behind, never a truncated destination, consistent cross-filesystem moves.
#include <map>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;

namespace {

// src/ with three top-level items; "big" spans several 64 KiB chunks.
void build_source(Fixture& fx) {
    fx.file("src/one/a.txt", "a");
    fx.file("src/one/sub/b.txt", std::string(1000, 'b'));
    fx.file("src/one/sub/big.bin", std::string(400000, 'g'));
    fx.link("src/one/sub/l", "b.txt");
    fx.file("src/two.bin", std::string(300000, 't'));
    fx.file("src/three/c.txt", "c");
}

std::vector<Path> sources(Fixture& fx) {
    return {fx / "src/one", fx / "src/two.bin", fx / "src/three"};
}

// Every regular file below `dst` equals its counterpart below `src` (complete copies only).
void check_complete_files(const Path& src, const Path& dst) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dst, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && !it->is_symlink(ec)) {
            const Path rel = it->path().lexically_relative(dst);
            CAPTURE(rel.native());
            CHECK(read_file(it->path()) == read_file(src / rel));
        }
    }
}

} // namespace

TEST_CASE("copy cancelled before each entry leaves only complete files", "[cancel]") {
    for (int k = 1; k <= 9; ++k) {
        CAPTURE(k);
        Fixture fx;
        Env env(fx);
        build_source(fx);
        const Path dst = fx.dir("dst");
        std::stop_source stop;
        int calls = 0;
        auto r = run(env, copy_req(sources(fx), dst), {}, stop.get_token(), [&](const Path&) {
            if (++calls == k) {
                stop.request_stop();
            }
            return true;
        });
        REQUIRE(r);
        CHECK(part_files(fx.root()).empty());
        check_complete_files(fx / "src", dst);
        if (calls >= k) {
            CHECK(r->cancelled);
        }
        // Undo of whatever completed brings the destination back to empty (all to the trash).
        if (r->record) {
            auto u = run(env, undo_req(record_of(*r)));
            REQUIRE(u);
            CHECK(count_entries(dst) == 0);
        }
    }
}

TEST_CASE("copy cancelled inside a file (between chunks) removes the temp file", "[cancel]") {
    for (std::uint64_t at : {0ULL, 65536ULL, 131072ULL, 327680ULL}) {
        CAPTURE(at);
        Fixture fx;
        Env env(fx);
        build_source(fx);
        const Path dst = fx.dir("dst");
        std::stop_source stop;
        ScopedIoHook hook([&](ops::detail::IoSite site, const Path& p, std::uint64_t off) -> int {
            if (site == ops::detail::IoSite::Write && p.filename() == "big.bin" && off == at) {
                stop.request_stop();
            }
            return 0;
        });
        auto r = run(env, copy_req(sources(fx), dst), {}, stop.get_token());
        REQUIRE(r);
        CHECK(r->cancelled);
        CHECK(part_files(fx.root()).empty());
        CHECK_FALSE(present(dst / "one/sub/big.bin"));
        check_complete_files(fx / "src", dst);
        // Directory metadata was still applied on the way out (no 0700 leftovers).
        if (present(dst / "one/sub")) {
            CHECK((lstat_of(dst / "one/sub").st_mode & 0777) ==
                  (lstat_of(fx / "src/one/sub").st_mode & 0777));
        }
    }
}

TEST_CASE("cross-filesystem move cancelled at many points stays consistent", "[cancel][xfs]") {
    for (int k = 1; k <= 9; ++k) {
        CAPTURE(k);
        Fixture fx;
        Env env(fx);
        build_source(fx);
        std::map<std::string, std::string> digests;
        for (const Path& s : sources(fx)) {
            digests[s.filename().native()] =
                present(s) && S_ISDIR(lstat_of(s).st_mode) ? tree_digest(s) : read_file(s);
        }
        const Path dst = fx.dir("dst");
        std::stop_source stop;
        int calls = 0;
        ScopedIoHook hook([](ops::detail::IoSite site, const Path&, std::uint64_t) -> int {
            return site == ops::detail::IoSite::Rename ? EXDEV : 0;
        });
        auto r = run(env, move_req(sources(fx), dst), {}, stop.get_token(), [&](const Path&) {
            if (++calls == k) {
                stop.request_stop();
            }
            return true;
        });
        REQUIRE(r);
        CHECK(part_files(fx.root()).empty());
        // Each top-level item is entirely at the source or entirely at the destination.
        for (const auto& [name, digest] : digests) {
            CAPTURE(name);
            const bool at_src = present(fx / "src" / name);
            const bool at_dst = present(dst / name);
            CHECK(at_src != at_dst);
            const Path where = at_src ? fx / "src" / name : dst / name;
            const std::string now =
                S_ISDIR(lstat_of(where).st_mode) ? tree_digest(where) : read_file(where);
            CHECK(now == digest);
        }
    }
}

TEST_CASE("ENOSPC / EIO mid-copy fail the item without a truncated file", "[fault]") {
    for (int err : {ENOSPC, EIO, EDQUOT}) {
        CAPTURE(err);
        Fixture fx;
        Env env(fx);
        build_source(fx);
        const Path dst = fx.dir("dst");
        ScopedIoHook hook([&](ops::detail::IoSite site, const Path& p, std::uint64_t off) -> int {
            return site == ops::detail::IoSite::Write && p.filename() == "two.bin" && off >= 131072
                       ? err
                       : 0;
        });
        auto r = run(env, copy_req(sources(fx), dst));
        REQUIRE(r);
        REQUIRE(r->items.size() == 3);
        CHECK(r->items[0].status == ItemStatus::Done);
        CHECK(r->items[1].status == ItemStatus::Failed);
        CHECK(code_of(r->items[1]) == (err == EIO ? ErrorCode::IoError : ErrorCode::NoSpace));
        CHECK(r->items[2].status == ItemStatus::Done);
        CHECK_FALSE(present(dst / "two.bin"));
        CHECK(part_files(fx.root()).empty());
        check_complete_files(fx / "src", dst);
    }
}

TEST_CASE("EACCES: injected at create, and a real read-only destination", "[fault]") {
    Fixture fx;
    Env env(fx);
    build_source(fx);
    {
        const Path dst = fx.dir("dst");
        ScopedIoHook hook([](ops::detail::IoSite site, const Path& p, std::uint64_t) -> int {
            return site == ops::detail::IoSite::CreateTemp && p.filename() == "b.txt" ? EACCES : 0;
        });
        auto r = run(env, copy_req(sources(fx), dst));
        REQUIRE(r);
        CHECK(r->items[0].status == ItemStatus::Done);
        CHECK(r->items[0].partial);
        CHECK(code_of(r->items[0]) == ErrorCode::PermissionDenied);
        CHECK_FALSE(present(dst / "one/sub/b.txt"));
        CHECK(read_file(dst / "one/sub/big.bin").size() == 400000);
    }
    if (::geteuid() != 0) {
        const Path ro = fx.dir("ro");
        ::chmod(ro.c_str(), 0555);
        auto r = run(env, copy_req(sources(fx), ro));
        REQUIRE(r);
        CHECK(r->failed == 3);
        for (const ItemOutcome& o : r->items) {
            CHECK(code_of(o) == ErrorCode::PermissionDenied);
        }
        ::chmod(ro.c_str(), 0755);
        CHECK(count_entries(ro) == 0);
    }
    CHECK(part_files(fx.root()).empty());
}

TEST_CASE("fsync and commit failures remove the temp file", "[fault]") {
    for (ops::detail::IoSite site : {ops::detail::IoSite::Fsync, ops::detail::IoSite::Commit}) {
        Fixture fx;
        Env env(fx);
        fx.file("src/a", "a");
        const Path dst = fx.dir("dst");
        ScopedIoHook hook([&](ops::detail::IoSite s, const Path&, std::uint64_t) -> int {
            return s == site ? EIO : 0;
        });
        OpRequest req = copy_req({fx / "src/a"}, dst);
        req.options.sync_copies = true;
        auto r = run(env, req);
        REQUIRE(r);
        CHECK(r->items[0].status == ItemStatus::Failed);
        CHECK(count_entries(dst) == 0);
        CHECK(part_files(fx.root()).empty());
    }
}

TEST_CASE("the fault injector produces partial failures inside trees", "[fault]") {
    Fixture fx;
    Env env(fx);
    build_source(fx);
    const Path dst = fx.dir("dst");
    auto r = run(env, copy_req(sources(fx), dst), {}, {},
                 [](const Path& p) { return p.filename() != "sub"; });
    REQUIRE(r);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(r->items[0].partial);
    CHECK(error_of(r->items[0]).message.find("injected") != std::string::npos);
    CHECK(read_file(dst / "one/a.txt") == "a");
    CHECK_FALSE(present(dst / "one/sub"));
    // Top-level injection fails just that item.
    auto r2 = run(env, copy_req(sources(fx), fx.dir("dst2")), {}, {},
                  [](const Path& p) { return p.filename() != "two.bin"; });
    REQUIRE(r2);
    CHECK(r2->items[1].status == ItemStatus::Failed);
    CHECK(r2->failed == 1);
}
