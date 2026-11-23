// Trash (S2), Restore, confirmed DeletePermanently (S1a, S3, S4, S5). Fake trash in the fixture.
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;

TEST_CASE("trash moves items into the trash and records them", "[trash]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("d/a.txt", "a");
    fx.file("d/t/x", "x");
    fx.link("d/l", "/nonexistent-target");
    auto r = run(env, trash_req({a, fx / "d/t", fx / "d/l"}));
    REQUIRE(r);
    CHECK(r->done == 3);
    CHECK(r->untrashable.empty());
    CHECK_FALSE(present(a));
    CHECK(env.trash->item_count() == 3);
    REQUIRE(r->record);
    REQUIRE(record_of(*r).steps.size() == 3);
    const UndoStep& s = record_of(*r).steps[0];
    CHECK(s.kind == OpKind::Trash);
    CHECK(s.before == a);
    REQUIRE(s.trashed);
    CHECK(s.after.path == trashed_of(s).files_path());
    CHECK(read_file(trashed_of(s).files_path()) == "a");
    CHECK(r->items[0].destination == trashed_of(s).files_path());
    // The link was trashed as a link (T2).
    CHECK(S_ISLNK(lstat_of(trashed_of(record_of(*r).steps[2]).files_path()).st_mode));
}

TEST_CASE("untrashable items fail with Unsupported, are listed, and are never deleted (S2)",
          "[trash]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("nofs/a", "a");
    const Path b = fx.file("ok/b", "b");
    env.trash->set_unsupported({fx / "nofs"});
    auto r = run(env, trash_req({a, b}));
    REQUIRE(r);
    REQUIRE(r->items.size() == 2);
    CHECK(r->items[0].status == ItemStatus::Failed);
    CHECK(code_of(r->items[0]) == ErrorCode::Unsupported);
    CHECK(r->untrashable == std::vector<Path>{a});
    CHECK(read_file(a) == "a"); // still there
    CHECK(r->items[1].status == ItemStatus::Done);

    // No trash service at all: every item untrashable, nothing removed.
    Env none(fx, false);
    const Path c = fx.file("ok/c", "c");
    auto r2 = run(none, trash_req({c}));
    REQUIRE(r2);
    CHECK(r2->items[0].status == ItemStatus::Failed);
    CHECK(r2->untrashable == std::vector<Path>{c});
    CHECK(read_file(c) == "c");

    // dry_run reports the same without moving anything.
    OpRequest dry = trash_req({a, fx / "ok/c"});
    dry.options.dry_run = true;
    auto r3 = run(env, dry);
    REQUIRE(r3);
    CHECK(r3->untrashable == std::vector<Path>{a});
    CHECK(present(fx / "ok/c"));
}

TEST_CASE("restore puts items back, creating missing folders, conflicts ask", "[trash][restore]") {
    Fixture fx;
    Env env(fx);
    const Path a = fx.file("deep/er/a.txt", "a");
    const Path b = fx.file("b.txt", "b");
    auto t = run(env, trash_req({a, b}));
    REQUIRE(t);
    const trash::TrashedItem ia = trashed_of(record_of(*t).steps[0]);
    const trash::TrashedItem ib = trashed_of(record_of(*t).steps[1]);
    std::filesystem::remove_all(fx / "deep"); // the original folder is gone
    fx.file("b.txt", "new b");                // and the other name is taken

    OpRequest req;
    req.kind = OpKind::Restore;
    req.trashed = {ia, ib};
    Answer ans({ConflictAction::KeepBoth, false, {}});
    auto r = run(env, req, ans.resolver());
    REQUIRE(r);
    CHECK(r->done == 2);
    CHECK(read_file(a) == "a");
    CHECK(read_file(fx / "b (2).txt") == "b");
    CHECK(read_file(b) == "new b");
    REQUIRE(ans.seen.size() == 1);
    CHECK_FALSE(ans.seen[0].merge_allowed);
    REQUIRE(r->record);
    const UndoStep& s = record_of(*r).steps[0];
    CHECK(s.kind == OpKind::Restore);
    REQUIRE(s.created_parents.size() == 2);
    CHECK(s.created_parents[0].path == fx / "deep");
    CHECK(s.created_parents[1].path == fx / "deep/er");
    // Undo of the restore: back to the trash, created folders removed while empty.
    auto u = run(env, undo_req(record_of(*r)));
    REQUIRE(u);
    CHECK(u->done == 2);
    CHECK_FALSE(present(fx / "deep"));
    CHECK_FALSE(present(fx / "b (2).txt"));
    CHECK(read_file(b) == "new b");
}

TEST_CASE("permanent delete requires confirmed and removes trees without following links",
          "[delete][symlink]") {
    Fixture fx;
    Fixture sentinel_fx;
    Env env(fx);
    sentinel_fx.file("sentinel/a.txt", "precious");
    sentinel_fx.file("sentinel/sub/b.txt", "precious too");
    const std::string before = tree_digest(sentinel_fx.root());
    fx.file("victim/x.txt", "x");
    fx.file("victim/deep/y.txt", "y");
    fx.link("victim/link-dir", (sentinel_fx / "sentinel").native());
    fx.link("victim/deep/link-file", (sentinel_fx / "sentinel/a.txt").native());
    fx.file("victim/ro/z", "z");
    ::chmod((fx / "victim/ro/z").c_str(), 0444);
    const Path top_link = fx.link("top-link", (sentinel_fx / "sentinel").native());

    OpRequest unconfirmed = delete_req({fx / "victim"});
    unconfirmed.confirmed = false;
    CHECK(run(env, unconfirmed).error().code == ErrorCode::InvalidArgument);
    CHECK(present(fx / "victim/x.txt"));

    auto r = run(env, delete_req({fx / "victim", top_link}));
    REQUIRE(r);
    CHECK(r->done == 2);
    CHECK_FALSE(present(fx / "victim"));
    CHECK_FALSE(present(top_link));
    CHECK(tree_digest(sentinel_fx.root()) == before); // nothing behind the links touched
    CHECK_FALSE(r->record);                           // not undoable
    CHECK(env.trash->item_count() == 0);
}

TEST_CASE("permanent delete: injected failures keep their folders; partial reported",
          "[delete][fault]") {
    Fixture fx;
    Env env(fx);
    fx.file("v/keep/stuck.txt", "s");
    fx.file("v/gone/a", "a");
    fx.file("v/b", "b");
    auto r = run(env, delete_req({fx / "v"}), {}, {},
                 [](const Path& p) { return p.filename() != "stuck.txt"; });
    REQUIRE(r);
    REQUIRE(r->items.size() == 1);
    CHECK(r->items[0].status == ItemStatus::Failed);
    CHECK(r->items[0].partial);
    CHECK(code_of(r->items[0]) == ErrorCode::IoError);
    CHECK(read_file(fx / "v/keep/stuck.txt") == "s");
    CHECK_FALSE(present(fx / "v/gone"));
    CHECK_FALSE(present(fx / "v/b"));
}

TEST_CASE("permanent delete of trashed items (trash.empty) forgets their info files",
          "[delete][trash]") {
    Fixture fx;
    Env env(fx);
    fx.file("a", "a");
    fx.file("t/x", "x");
    auto t = run(env, trash_req({fx / "a", fx / "t"}));
    REQUIRE(t);
    OpRequest req;
    req.kind = OpKind::DeletePermanently;
    req.confirmed = true;
    for (const UndoStep& s : record_of(*t).steps) {
        req.trashed.push_back(trashed_of(s));
    }
    auto r = run(env, req);
    REQUIRE(r);
    CHECK(r->done == 2);
    CHECK(env.trash->item_count() == 0);
    CHECK_FALSE(present(req.trashed[0].info_path()));
    CHECK_FALSE(present(req.trashed[1].info_path()));
    CHECK_FALSE(r->record);

    // A stale listing (identity changed) never deletes a different item.
    fx.file("c", "c");
    auto t2 = run(env, trash_req({fx / "c"}));
    REQUIRE(t2);
    trash::TrashedItem stale = trashed_of(record_of(*t2).steps[0]);
    stale.id.inode += 12345;
    OpRequest req2;
    req2.kind = OpKind::DeletePermanently;
    req2.confirmed = true;
    req2.trashed = {stale};
    auto r2 = run(env, req2);
    REQUIRE(r2);
    CHECK(r2->items[0].status == ItemStatus::Failed);
    CHECK(read_file(stale.files_path()) == "c");
}

TEST_CASE("make directory and symlink, with conflicts", "[mkdir][symlink]") {
    Fixture fx;
    Env env(fx);
    const Path parent = fx.dir("p");
    auto r = run(env, mkdir_req(parent, "Yeni Klasör"));
    REQUIRE(r);
    CHECK(r->done == 1);
    struct stat st = lstat_of(parent / "Yeni Klasör");
    CHECK(S_ISDIR(st.st_mode));
    REQUIRE(r->record);
    CHECK(record_of(*r).steps[0].kind == OpKind::MakeDirectory);
    CHECK(record_of(*r).steps[0].before.empty());

    Answer keep({ConflictAction::KeepBoth, false, {}});
    auto r2 = run(env, mkdir_req(parent, "Yeni Klasör"), keep.resolver());
    REQUIRE(r2);
    REQUIRE(keep.seen.size() == 1);
    CHECK_FALSE(keep.seen[0].replace_allowed);
    CHECK_FALSE(keep.seen[0].merge_allowed);
    CHECK(present(parent / "Yeni Klasör (2)"));
    auto r3 = run(env, mkdir_req(parent, "Yeni Klasör", ConflictPolicy::Skip));
    REQUIRE(r3);
    CHECK(r3->skipped == 1);

    const Path target = fx.file("target.txt", "t");
    OpRequest ln;
    ln.kind = OpKind::CreateSymlink;
    ln.sources = {target};
    ln.destination = parent;
    auto r4 = run(env, ln);
    REQUIRE(r4);
    CHECK(r4->done == 1);
    CHECK(S_ISLNK(lstat_of(parent / "target.txt").st_mode));
    CHECK(std::filesystem::read_symlink(parent / "target.txt") == target);
    REQUIRE(r4->record);
    CHECK(record_of(*r4).steps[0].before == target);
    Answer repl({ConflictAction::Replace, false, {}});
    auto r5 = run(env, ln, repl.resolver());
    REQUIRE(r5);
    CHECK(r5->done == 1);
    CHECK(env.trash->item_count() == 1); // the old link went to the trash
}
