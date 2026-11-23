// Conflicts (S8): every policy, every decision, apply_to_all, KeepBoth names, Replace through
// the trash, permanent replace only for files, Merge, Cancel. Fixtures only, fake trash.
#include <array>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;

namespace {

void set_mtime(const Path& p, std::int64_t sec) {
    const std::array<struct timespec, 2> t{timespec{sec, 0}, timespec{sec, 0}};
    REQUIRE(::utimensat(AT_FDCWD, p.c_str(), t.data(), AT_SYMLINK_NOFOLLOW) == 0);
}

struct Scene {
    Fixture fx;
    Env env{fx};
    Path src;
    Path dst;
    Scene() {
        src = fx.dir("src");
        dst = fx.dir("dst");
        fx.file("src/a.txt", "new a");
        fx.file("src/b.txt", "new b");
        fx.file("dst/a.txt", "old a");
        fx.file("dst/b.txt", "old b");
    }
};

} // namespace

TEST_CASE("policies for file onto file: Skip, KeepBoth, Replace", "[conflict]") {
    for (OpKind kind : {OpKind::Copy, OpKind::Move}) {
        CAPTURE(to_string(kind));
        SECTION(std::string(to_string(kind)) + " Skip") {
            Scene s;
            OpRequest req = copy_req({s.src / "a.txt"}, s.dst, ConflictPolicy::Skip);
            req.kind = kind;
            auto r = run(s.env, req);
            REQUIRE(r);
            CHECK(r->skipped == 1);
            CHECK(r->items[0].status == ItemStatus::Skipped);
            CHECK(read_file(s.dst / "a.txt") == "old a");
            CHECK(present(s.src / "a.txt"));
            CHECK_FALSE(r->record);
        }
        SECTION(std::string(to_string(kind)) + " KeepBoth") {
            Scene s;
            s.fx.file("dst/a (2).txt", "taken");
            OpRequest req = copy_req({s.src / "a.txt"}, s.dst, ConflictPolicy::KeepBoth);
            req.kind = kind;
            auto r = run(s.env, req);
            REQUIRE(r);
            CHECK(r->done == 1);
            CHECK(r->items[0].destination == s.dst / "a (3).txt");
            CHECK(read_file(s.dst / "a (3).txt") == "new a");
            CHECK(read_file(s.dst / "a.txt") == "old a");
            CHECK(present(s.src / "a.txt") == (kind == OpKind::Copy));
        }
        SECTION(std::string(to_string(kind)) + " Replace trashes the old file first") {
            Scene s;
            OpRequest req = copy_req({s.src / "a.txt"}, s.dst, ConflictPolicy::Replace);
            req.kind = kind;
            auto r = run(s.env, req);
            REQUIRE(r);
            CHECK(r->done == 1);
            CHECK(read_file(s.dst / "a.txt") == "new a");
            REQUIRE(r->record);
            REQUIRE(record_of(*r).steps.size() == 1);
            const UndoStep& st = record_of(*r).steps[0];
            REQUIRE(st.replaced);
            CHECK_FALSE(st.replaced_permanently);
            CHECK(read_file(replaced_of(st).files_path()) == "old a");
            CHECK(s.env.trash->item_count() == 1);
            CHECK_FALSE(record_of(*r).lossy);
        }
    }
}

TEST_CASE("ReplaceIfNewer replaces only older files and merges folders", "[conflict]") {
    Scene s;
    set_mtime(s.src / "a.txt", 2000000000); // newer than dst/a.txt
    set_mtime(s.dst / "a.txt", 1000000000);
    set_mtime(s.src / "b.txt", 1000000000); // older than dst/b.txt
    set_mtime(s.dst / "b.txt", 2000000000);
    s.fx.file("src/f/x", "nx");
    s.fx.file("src/f/y", "ny");
    s.fx.file("dst/f/x", "ox");
    auto r = run(s.env, copy_req({s.src / "a.txt", s.src / "b.txt", s.src / "f"}, s.dst,
                                 ConflictPolicy::ReplaceIfNewer));
    REQUIRE(r);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(r->items[1].status == ItemStatus::Skipped);
    CHECK(r->items[2].status == ItemStatus::Done); // merged
    CHECK(read_file(s.dst / "a.txt") == "new a");
    CHECK(read_file(s.dst / "b.txt") == "old b");
    CHECK(read_file(s.dst / "f/y") == "ny");
    // f/x: both files with equal (fresh) mtimes or older source: not replaced unless newer.
    CHECK(present(s.dst / "f/x"));
}

TEST_CASE("Ask: every decision for files", "[conflict][ask]") {
    SECTION("Skip") {
        Scene s;
        Answer ans({ConflictAction::Skip, false, {}});
        auto r = run(s.env, copy_req({s.src / "a.txt"}, s.dst), ans.resolver());
        REQUIRE(r);
        REQUIRE(ans.seen.size() == 1);
        const Conflict& c = ans.seen[0];
        CHECK(c.id.valid());
        CHECK(c.kind == OpKind::Copy);
        CHECK(c.source.path == s.src / "a.txt");
        CHECK(c.existing.path == s.dst / "a.txt");
        CHECK(c.existing.size == 5);
        CHECK(c.keep_both_name == "a (2).txt");
        CHECK(c.replace_allowed);
        CHECK_FALSE(c.replace_is_permanent);
        CHECK_FALSE(c.merge_allowed);
        CHECK_FALSE(c.same_file);
        CHECK(c.remaining_items == 1);
        CHECK(r->skipped == 1);
        CHECK(read_file(s.dst / "a.txt") == "old a");
    }
    SECTION("Replace") {
        Scene s;
        Answer ans({ConflictAction::Replace, false, {}});
        auto r = run(s.env, move_req({s.src / "a.txt"}, s.dst), ans.resolver());
        REQUIRE(r);
        CHECK(read_file(s.dst / "a.txt") == "new a");
        CHECK_FALSE(present(s.src / "a.txt"));
        CHECK(s.env.trash->item_count() == 1);
    }
    SECTION("KeepBoth with the suggested name") {
        Scene s;
        Answer ans({ConflictAction::KeepBoth, false, {}});
        auto r = run(s.env, copy_req({s.src / "a.txt"}, s.dst), ans.resolver());
        REQUIRE(r);
        CHECK(read_file(s.dst / "a (2).txt") == "new a");
    }
    SECTION("KeepBoth with a typed name; a taken typed name asks again") {
        Scene s;
        int calls = 0;
        ConflictResolver res = [&](const Conflict&) {
            ++calls;
            if (calls == 1) {
                return ConflictDecision{ConflictAction::KeepBoth, false, "b.txt"}; // taken
            }
            if (calls == 2) {
                return ConflictDecision{ConflictAction::KeepBoth, false, "bad/name"}; // invalid
            }
            return ConflictDecision{ConflictAction::KeepBoth, false, "Türkçe ad.txt"};
        };
        auto r = run(s.env, copy_req({s.src / "a.txt"}, s.dst), res);
        REQUIRE(r);
        CHECK(calls == 3);
        CHECK(read_file(s.dst / "Türkçe ad.txt") == "new a");
        CHECK(read_file(s.dst / "b.txt") == "old b");
    }
    SECTION("Cancel stops the whole operation") {
        Scene s;
        Answer ans({ConflictAction::Cancel, false, {}});
        s.fx.file("src/c.txt", "c");
        auto r = run(s.env, copy_req({s.src / "a.txt", s.src / "c.txt"}, s.dst), ans.resolver());
        REQUIRE(r);
        CHECK(r->cancelled);
        REQUIRE(r->items.size() == 2);
        CHECK(r->items[0].status == ItemStatus::Cancelled);
        CHECK(r->items[1].status == ItemStatus::Cancelled);
        CHECK_FALSE(present(s.dst / "c.txt"));
        CHECK(part_files(s.fx.root()).empty());
    }
    SECTION("Merge is not allowed for files: asked again, then Skip") {
        Scene s;
        Answer ans({ConflictAction::Merge, false, {}});
        auto r = run(s.env, copy_req({s.src / "a.txt"}, s.dst), ans.resolver());
        REQUIRE(r);
        CHECK(ans.seen.size() >= 2);
        CHECK(r->skipped == 1);
        CHECK(read_file(s.dst / "a.txt") == "old a");
    }
    SECTION("an empty resolver answers Skip") {
        Scene s;
        auto r = run(s.env, copy_req({s.src / "a.txt"}, s.dst));
        REQUIRE(r);
        CHECK(r->skipped == 1);
    }
}

TEST_CASE("apply_to_all answers later conflicts of the same shape only", "[conflict][ask]") {
    Scene s;
    s.fx.file("src/d1/x", "1");
    s.fx.file("src/d2/y", "2");
    s.fx.dir("dst/d1");
    s.fx.dir("dst/d2");
    int file_asks = 0;
    int dir_asks = 0;
    ConflictResolver res = [&](const Conflict& c) {
        if (c.merge_allowed) {
            ++dir_asks;
            return ConflictDecision{ConflictAction::Merge, true, {}};
        }
        ++file_asks;
        return ConflictDecision{ConflictAction::Replace, true, {}};
    };
    auto r =
        run(s.env, copy_req({s.src / "a.txt", s.src / "d1", s.src / "b.txt", s.src / "d2"}, s.dst),
            res);
    REQUIRE(r);
    CHECK(file_asks == 1);
    CHECK(dir_asks == 1);
    CHECK(r->done == 4);
    CHECK(read_file(s.dst / "a.txt") == "new a");
    CHECK(read_file(s.dst / "b.txt") == "new b");
    CHECK(read_file(s.dst / "d1/x") == "1");
    CHECK(read_file(s.dst / "d2/y") == "2");
}

TEST_CASE("Merge folders: nested conflicts ask again; undo steps only for new entries",
          "[conflict][merge]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/f/new.txt", "n");
    fx.file("src/f/same.txt", "src");
    fx.file("src/f/sub/deep.txt", "d");
    fx.file("dst/f/same.txt", "dst");
    fx.file("dst/f/mine.txt", "keep me");
    for (OpKind kind : {OpKind::Copy, OpKind::Move}) {
        (void)kind;
    }
    std::vector<std::string> asked;
    ConflictResolver res = [&](const Conflict& c) {
        asked.push_back(c.existing.path.filename().native());
        if (c.merge_allowed) {
            return ConflictDecision{ConflictAction::Merge, false, {}};
        }
        return ConflictDecision{ConflictAction::KeepBoth, false, {}};
    };
    auto r = run(env, copy_req({fx / "src/f"}, fx / "dst"), res);
    REQUIRE(r);
    CHECK(asked == std::vector<std::string>{"f", "same.txt"});
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(read_file(fx / "dst/f/new.txt") == "n");
    CHECK(read_file(fx / "dst/f/same.txt") == "dst");
    CHECK(read_file(fx / "dst/f/same (2).txt") == "src");
    CHECK(read_file(fx / "dst/f/sub/deep.txt") == "d");
    CHECK(read_file(fx / "dst/f/mine.txt") == "keep me");
    REQUIRE(r->record);
    // new.txt, same (2).txt, sub: the merged folder itself is not a step.
    CHECK(record_of(*r).steps.size() == 3);
    for (const UndoStep& st : record_of(*r).steps) {
        CHECK(st.after.path != fx / "dst/f");
    }
    // Undo puts exactly the new entries in the trash; the user's file stays.
    auto u = run(env, undo_req(record_of(*r)));
    REQUIRE(u);
    CHECK(u->done == 3);
    CHECK(read_file(fx / "dst/f/mine.txt") == "keep me");
    CHECK(read_file(fx / "dst/f/same.txt") == "dst");
    CHECK_FALSE(present(fx / "dst/f/new.txt"));
    CHECK_FALSE(present(fx / "dst/f/sub"));
}

TEST_CASE("Move merge moves children and leaves the emptied source folder", "[conflict][merge]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/f/a", "a");
    fx.file("src/f/b/c", "c");
    fx.file("dst/f/b/old", "o");
    auto r = run(env, move_req({fx / "src/f"}, fx / "dst"), [](const Conflict& c) {
        return ConflictDecision{
            c.merge_allowed ? ConflictAction::Merge : ConflictAction::Skip, true, {}};
    });
    REQUIRE(r);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(read_file(fx / "dst/f/a") == "a");
    CHECK(read_file(fx / "dst/f/b/c") == "c");
    CHECK(read_file(fx / "dst/f/b/old") == "o");
    CHECK_FALSE(present(fx / "src/f/a"));
    CHECK(present(fx / "src/f")); // S1: not removed
    REQUIRE(r->record);
    CHECK(record_of(*r).steps.size() == 2); // a, b/c
    auto u = run(env, undo_req(record_of(*r)));
    REQUIRE(u);
    CHECK(read_file(fx / "src/f/a") == "a");
    CHECK(read_file(fx / "src/f/b/c") == "c");
    CHECK(read_file(fx / "dst/f/b/old") == "o");
}

TEST_CASE("no trash: a file is replaced permanently and the record is lossy; a folder never",
          "[conflict][permanent]") {
    Fixture fx;
    Env env(fx, /*with_trash=*/false);
    fx.file("src/a.txt", "new");
    fx.file("dst/a.txt", "old");
    fx.file("src/d/x", "x");
    fx.file("dst/d/y", "y");
    Answer ans({ConflictAction::Replace, false, {}});
    auto r = run(env, copy_req({fx / "src/a.txt"}, fx / "dst"), ans.resolver());
    REQUIRE(r);
    REQUIRE(ans.seen.size() == 1);
    CHECK(ans.seen[0].replace_allowed);
    CHECK(ans.seen[0].replace_is_permanent);
    CHECK(read_file(fx / "dst/a.txt") == "new");
    REQUIRE(r->record);
    CHECK(record_of(*r).lossy);
    CHECK(record_of(*r).steps[0].replaced_permanently);

    std::vector<Conflict> seen;
    auto r2 = run(env, copy_req({fx / "src/d"}, fx / "dst"), [&](const Conflict& c) {
        seen.push_back(c);
        return ConflictDecision{ConflictAction::Replace, false, {}};
    });
    REQUIRE(r2);
    REQUIRE_FALSE(seen.empty());
    CHECK_FALSE(seen[0].replace_allowed);
    CHECK(seen[0].merge_allowed);
    CHECK(r2->items[0].status == ItemStatus::Skipped); // Replace refused -> asked again -> Skip
    CHECK(read_file(fx / "dst/d/y") == "y");

    // Policy Replace on a folder without trash fails the item instead of replacing.
    auto r3 = run(env, copy_req({fx / "src/d"}, fx / "dst", ConflictPolicy::Replace));
    REQUIRE(r3);
    CHECK(r3->items[0].status == ItemStatus::Failed);
    CHECK(read_file(fx / "dst/d/y") == "y");

    // trash_replaced = false behaves like a missing trash (file permanent, folder refused).
    Env with(fx);
    OpRequest req = copy_req({fx / "src/a.txt"}, fx / "dst", ConflictPolicy::Replace);
    req.options.trash_replaced = false;
    write_file(fx / "dst/a.txt", "old2");
    auto r4 = run(with, req);
    REQUIRE(r4);
    CHECK(read_file(fx / "dst/a.txt") == "new");
    CHECK(record_of(*r4).lossy);
    CHECK(with.trash->item_count() == 0);
}

TEST_CASE("Replace of a folder with a trash moves the old folder to the trash", "[conflict]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/d/new", "n");
    fx.file("dst/d/old", "o");
    auto r = run(env, copy_req({fx / "src/d"}, fx / "dst", ConflictPolicy::Replace));
    REQUIRE(r);
    CHECK(r->done == 1);
    CHECK(read_file(fx / "dst/d/new") == "n");
    CHECK_FALSE(present(fx / "dst/d/old"));
    REQUIRE(record_of(*r).steps[0].replaced);
    CHECK(read_file(replaced_of(record_of(*r).steps[0]).files_path() / "old") == "o");
    // Undo: the copy goes to the trash, the replaced folder comes back.
    auto u = run(env, undo_req(record_of(*r)));
    REQUIRE(u);
    CHECK(read_file(fx / "dst/d/old") == "o");
    CHECK_FALSE(present(fx / "dst/d/new"));
}

TEST_CASE("same file (hard link) conflicts offer only Skip / KeepBoth", "[conflict]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("src/a", "same");
    fx.dir("dst");
    REQUIRE(::link(a.c_str(), (fx / "dst/a").c_str()) == 0);
    Answer ans({ConflictAction::Replace, false, {}});
    auto r = run(env, copy_req({a}, fx / "dst"), ans.resolver());
    REQUIRE(r);
    REQUIRE_FALSE(ans.seen.empty());
    CHECK(ans.seen[0].same_file);
    CHECK_FALSE(ans.seen[0].replace_allowed);
    CHECK(r->skipped == 1);
    CHECK(read_file(a) == "same");
    CHECK(env.trash->item_count() == 0);
}

TEST_CASE("Replace never trashes a folder that contains the source", "[conflict]") {
    Fixture fx;
    Env env(fx);
    fx.file("x/x/inner", "i"); // move x/x into its grandparent: name "x" is taken by x itself
    Answer ans({ConflictAction::Replace, false, {}});
    auto r = run(env, move_req({fx / "x/x"}, fx.root()), ans.resolver());
    REQUIRE(r);
    REQUIRE_FALSE(ans.seen.empty());
    CHECK_FALSE(ans.seen[0].replace_allowed);
    CHECK(read_file(fx / "x/x/inner") == "i");
    CHECK(env.trash->item_count() == 0);
}
