// Unicode / Turkish / invalid UTF-8 names through every kind, deep trees beyond the descriptor
// cap, many small files. Fixtures only.
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <sys/resource.h>
#include <sys/stat.h>

#include "dir_stack.hpp"
#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;

TEST_CASE("Turkish, NFD and invalid UTF-8 names survive copy, move, trash, undo", "[unicode]") {
    Fixture fx;
    Env env(fx);
    const std::vector<std::string> names = {
        "İstanbul ığüşöç ĞÜŞİÖÇ.txt",
        "e\xCC\x81t\xC3\xA9 nfd-nfc.txt", // NFD é then NFC é: two distinct names
        "bad \xff\xfe bytes.bin",         "emoji \xF0\x9F\x93\x81 klasör", " leading and trailing ",
    };
    std::vector<Path> srcs;
    srcs.reserve(names.size() + 1);
    for (const std::string& n : names) {
        srcs.push_back(fx.file("src/" + n, "content of " + n));
    }
    fx.file("src/Yeni Klasör/iç dosya.txt", "iç");
    srcs.push_back(fx / "src/Yeni Klasör");
    const std::string digest = tree_digest(fx / "src");

    auto c = run(env, copy_req(srcs, fx.dir("copy")));
    REQUIRE(c);
    CHECK(c->done == srcs.size());
    CHECK(tree_digest(fx / "copy") == digest);

    auto m = run(env, move_req(srcs, fx.dir("moved")));
    REQUIRE(m);
    CHECK(m->done == srcs.size());
    CHECK(tree_digest(fx / "moved") == digest);
    auto um = run(env, undo_req(record_of(*m)));
    REQUIRE(um);
    CHECK(tree_digest(fx / "src") == digest);

    auto t = run(env, trash_req(srcs));
    REQUIRE(t);
    CHECK(t->done == srcs.size());
    auto ut = run(env, undo_req(record_of(*t)));
    REQUIRE(ut);
    CHECK(ut->done == srcs.size());
    CHECK(tree_digest(fx / "src") == digest);

    // KeepBoth with a long Turkish name stays within NAME_MAX.
    std::string longname;
    while (longname.size() < 248) {
        longname += "ş";
    }
    const Path lf = fx.file("long/" + longname, "x");
    auto k1 = run(env, copy_req({lf}, fx / "long"));
    REQUIRE(k1);
    REQUIRE(k1->items[0].status == ItemStatus::Done);
    CHECK(k1->items[0].destination.filename().native().size() <= kNameMaxBytes);
    CHECK(read_file(k1->items[0].destination) == "x");
}

TEST_CASE("deep trees deeper than the descriptor cap copy, move and delete", "[deep]") {
    Fixture fx;
    Env env(fx);
    const std::size_t depth =
        ops::detail::DirStack::kMaxOpen * 3 + 7; // forces closes and ".." reopens
    std::string rel = "src/deep";
    for (std::size_t i = 0; i < depth; ++i) {
        rel += "/d" + std::to_string(i % 10);
        if (i % 17 == 0) {
            fx.file(rel + "/f.txt", "level " + std::to_string(i));
        }
    }
    fx.file(rel + "/leaf.txt", "leaf");
    const std::string digest = tree_digest(fx / "src/deep");
    auto c = run(env, copy_req({fx / "src/deep"}, fx.dir("dst")));
    REQUIRE(c);
    CHECK(c->done == 1);
    CHECK_FALSE(c->items[0].partial);
    CHECK(tree_digest(fx / "dst/deep") == digest);

    // Cross-filesystem move semantics through the seam (scan + copy + bottom-up removal).
    {
        ScopedIoHook hook([](ops::detail::IoSite s, const Path&, std::uint64_t) -> int {
            return s == ops::detail::IoSite::Rename ? EXDEV : 0;
        });
        auto m = run(env, move_req({fx / "dst/deep"}, fx.dir("moved")));
        REQUIRE(m);
        CHECK(m->done == 1);
        CHECK_FALSE(m->items[0].partial);
        CHECK(tree_digest(fx / "moved/deep") == digest);
        CHECK_FALSE(present(fx / "dst/deep"));
    }
    auto d = run(env, delete_req({fx / "moved/deep"}));
    REQUIRE(d);
    CHECK(d->done == 1);
    CHECK_FALSE(present(fx / "moved/deep"));
}

TEST_CASE("a deep walk works with a low descriptor limit", "[deep]") {
    Fixture fx;
    Env env(fx);
    std::string rel = "src/t";
    for (int i = 0; i < 120; ++i) {
        rel += "/x";
    }
    fx.file(rel + "/f", "f");
    rlimit old{};
    REQUIRE(::getrlimit(RLIMIT_NOFILE, &old) == 0);
    rlimit low = old;
    low.rlim_cur = 128; // < 2 cursors x 120 levels, > 2 x kMaxOpen (48) + stdio
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &low) == 0);
    auto c = run(env, copy_req({fx / "src/t"}, fx.dir("dst")));
    ::setrlimit(RLIMIT_NOFILE, &old);
    REQUIRE(c);
    CHECK(c->done == 1);
    CHECK_FALSE(c->items[0].partial);
    CHECK(read_file(fx / "dst/t" / std::string(rel).substr(6) / "f") == "f");
}

TEST_CASE("many small files: copy, then undo to the trash, then permanent delete", "[many]") {
    Fixture fx;
    Env env(fx);
    constexpr int kDirs = 20;
    constexpr int kFiles = 150;
    for (int d = 0; d < kDirs; ++d) {
        for (int f = 0; f < kFiles; ++f) {
            fx.file("src/m/d" + std::to_string(d) + "/f" + std::to_string(f),
                    std::to_string(d * 1000 + f));
        }
    }
    const std::string digest = tree_digest(fx / "src/m");
    auto c = run(env, copy_req({fx / "src/m"}, fx.dir("dst")));
    REQUIRE(c);
    CHECK(tree_digest(fx / "dst/m") == digest);
    auto u = run(env, undo_req(record_of(*c)));
    REQUIRE(u);
    CHECK_FALSE(present(fx / "dst/m"));
    auto d = run(env, delete_req({fx / "src/m"}));
    REQUIRE(d);
    CHECK(d->done == 1);
    CHECK_FALSE(present(fx / "src/m"));
}
