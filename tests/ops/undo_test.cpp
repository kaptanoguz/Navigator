// Undo / redo (S12): round trips for every undoable kind, never deleting, never overwriting,
// skipping changed items; permanent delete is not undoable. Fixtures only, fake trash.
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

// Forward -> undo -> redo -> undo, checking the state after each.
struct Trip {
    Env& env;
    UndoStack stack;
    explicit Trip(Env& e) : env(e) {}

    OpResult forward(const OpRequest& req, const ConflictResolver& res = {}) {
        auto r = run(env, req, res);
        REQUIRE(r);
        REQUIRE(r->record);
        stack.record_done(record_of(*r), OpOrigin::User);
        return *r;
    }
    OpResult undo(const ConflictResolver& res = {}) {
        auto rec = stack.take_undo();
        REQUIRE(rec);
        OpRequest req;
        req.kind = OpKind::Undo;
        req.origin = OpOrigin::Undo;
        req.record = rec;
        auto r = run(env, req, res);
        REQUIRE(r);
        if (r->record) {
            stack.record_done(record_of(*r), OpOrigin::Undo);
        }
        return *r;
    }
    OpResult redo(const ConflictResolver& res = {}) {
        auto rec = stack.take_redo();
        REQUIRE(rec);
        OpRequest req;
        req.kind = OpKind::Redo;
        req.origin = OpOrigin::Redo;
        req.record = rec;
        auto r = run(env, req, res);
        REQUIRE(r);
        if (r->record) {
            stack.record_done(record_of(*r), OpOrigin::Redo);
        }
        return *r;
    }
};

} // namespace

TEST_CASE("copy: undo trashes the copy, redo copies again", "[undo]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/a.txt", "a");
    fx.file("src/t/x", "x");
    Trip trip(env);
    trip.forward(copy_req({fx / "src/a.txt", fx / "src/t"}, fx.dir("dst")));
    const OpResult u = trip.undo();
    CHECK(u.done == 2);
    CHECK_FALSE(present(fx / "dst/a.txt"));
    CHECK_FALSE(present(fx / "dst/t"));
    CHECK(env.trash->item_count() == 2); // never deleted (S12)
    CHECK(read_file(fx / "src/a.txt") == "a");
    REQUIRE(u.record);
    CHECK(record_of(u).kind == OpKind::Copy);
    const OpResult rd = trip.redo();
    CHECK(rd.done == 2);
    CHECK(read_file(fx / "dst/a.txt") == "a");
    CHECK(read_file(fx / "dst/t/x") == "x");
    CHECK(trip.undo().done == 2);
    CHECK_FALSE(present(fx / "dst/a.txt"));
}

TEST_CASE("move: undo moves back, redo moves again", "[undo]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/a.txt", "a");
    fx.file("src/d/x", "x");
    Trip trip(env);
    trip.forward(move_req({fx / "src/a.txt", fx / "src/d"}, fx.dir("dst")));
    CHECK_FALSE(present(fx / "src/a.txt"));
    CHECK(trip.undo().done == 2);
    CHECK(read_file(fx / "src/a.txt") == "a");
    CHECK(read_file(fx / "src/d/x") == "x");
    CHECK_FALSE(present(fx / "dst/a.txt"));
    CHECK(trip.redo().done == 2);
    CHECK(read_file(fx / "dst/d/x") == "x");
    CHECK(trip.undo().done == 2);
    CHECK(read_file(fx / "src/d/x") == "x");
    CHECK(env.trash->item_count() == 0);
}

TEST_CASE("move with Replace: undo brings the replaced item back from the trash", "[undo]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/a.txt", "new");
    fx.file("dst/a.txt", "old");
    Trip trip(env);
    trip.forward(move_req({fx / "src/a.txt"}, fx / "dst", ConflictPolicy::Replace));
    CHECK(read_file(fx / "dst/a.txt") == "new");
    CHECK(trip.undo().done == 1);
    CHECK(read_file(fx / "src/a.txt") == "new");
    CHECK(read_file(fx / "dst/a.txt") == "old");
    CHECK(env.trash->item_count() == 0);
    // Redo replaces the same old file again (its identity matches) without asking.
    Answer ans({ConflictAction::Skip, false, {}});
    CHECK(trip.redo(ans.resolver()).done == 1);
    CHECK(ans.seen.empty());
    CHECK(read_file(fx / "dst/a.txt") == "new");
}

TEST_CASE("rename, make directory, symlink, trash, restore round trips", "[undo]") {
    Fixture fx;
    Env env(fx);
    Trip trip(env);
    SECTION("rename") {
        fx.file("d/a.txt", "a");
        trip.forward(rename_req(fx / "d/a.txt", "b.txt"));
        CHECK(trip.undo().done == 1);
        CHECK(read_file(fx / "d/a.txt") == "a");
        CHECK(trip.redo().done == 1);
        CHECK(read_file(fx / "d/b.txt") == "a");
        CHECK_FALSE(present(fx / "d/a.txt"));
    }
    SECTION("make directory") {
        fx.dir("p");
        trip.forward(mkdir_req(fx / "p", "new"));
        CHECK(trip.undo().done == 1);
        CHECK_FALSE(present(fx / "p/new"));
        CHECK(env.trash->item_count() == 0); // rmdir of our own empty folder (S1c)
        CHECK(trip.redo().done == 1);
        CHECK(present(fx / "p/new"));
    }
    SECTION("symlink") {
        const Path target = fx.file("t.txt", "t");
        fx.dir("p");
        OpRequest ln;
        ln.kind = OpKind::CreateSymlink;
        ln.sources = {target};
        ln.destination = fx / "p";
        trip.forward(ln);
        CHECK(trip.undo().done == 1);
        CHECK_FALSE(present(fx / "p/t.txt"));
        CHECK(env.trash->item_count() == 1);
        CHECK(trip.redo().done == 1);
        CHECK(std::filesystem::read_symlink(fx / "p/t.txt") == target);
        CHECK(read_file(target) == "t");
    }
    SECTION("trash") {
        fx.file("d/a.txt", "a");
        trip.forward(trash_req({fx / "d/a.txt"}));
        CHECK(trip.undo().done == 1);
        CHECK(read_file(fx / "d/a.txt") == "a");
        CHECK(env.trash->item_count() == 0);
        CHECK(trip.redo().done == 1);
        CHECK_FALSE(present(fx / "d/a.txt"));
        CHECK(env.trash->item_count() == 1);
        CHECK(trip.undo().done == 1);
        CHECK(read_file(fx / "d/a.txt") == "a");
    }
    SECTION("restore") {
        fx.file("d/a.txt", "a");
        auto t = run(env, trash_req({fx / "d/a.txt"}));
        REQUIRE(t);
        OpRequest req;
        req.kind = OpKind::Restore;
        req.trashed = {trashed_of(record_of(*t).steps[0])};
        trip.forward(req);
        CHECK(read_file(fx / "d/a.txt") == "a");
        CHECK(trip.undo().done == 1);
        CHECK_FALSE(present(fx / "d/a.txt"));
        CHECK(env.trash->item_count() == 1);
        CHECK(trip.redo().done == 1);
        CHECK(read_file(fx / "d/a.txt") == "a");
        CHECK(env.trash->item_count() == 0);
    }
}

TEST_CASE("undo never overwrites: an item that appeared becomes a conflict", "[undo][conflict]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/a.txt", "a");
    Trip trip(env);
    trip.forward(move_req({fx / "src/a.txt"}, fx.dir("dst")));
    fx.file("src/a.txt", "someone else's"); // appeared at the path undo would restore
    Answer ans({ConflictAction::Skip, false, {}});
    const OpResult u = trip.undo(ans.resolver());
    REQUIRE(ans.seen.size() == 1);
    CHECK(ans.seen[0].kind == OpKind::Undo);
    CHECK_FALSE(ans.seen[0].replace_is_permanent);
    CHECK(u.skipped == 1);
    CHECK(read_file(fx / "src/a.txt") == "someone else's");
    CHECK(read_file(fx / "dst/a.txt") == "a");
    CHECK_FALSE(u.record); // nothing was undone

    // Without a trash, Replace is not even offered during undo (never permanent).
    Env none(fx, false);
    UndoRecord rec;
    rec.kind = OpKind::Move;
    UndoStep st;
    st.kind = OpKind::Move;
    st.before = fx / "src/a.txt";
    st.after = ops::detail::guard_from(fx / "dst/a.txt", lstat_of(fx / "dst/a.txt"));
    rec.steps = {st};
    Answer repl({ConflictAction::Replace, false, {}});
    auto r = run(none, undo_req(rec), repl.resolver());
    REQUIRE(r);
    REQUIRE_FALSE(repl.seen.empty());
    CHECK_FALSE(repl.seen[0].replace_allowed);
    CHECK(read_file(fx / "src/a.txt") == "someone else's");
}

TEST_CASE("undo skips items that changed since (ItemStatus::Changed)", "[undo][changed]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/a.txt", "a");
    fx.file("src/b.txt", "b");
    Trip trip(env);
    trip.forward(copy_req({fx / "src/a.txt", fx / "src/b.txt"}, fx.dir("dst")));
    write_file(fx / "dst/a.txt", "edited by the user"); // changed since the copy
    const OpResult u = trip.undo();
    CHECK(u.changed == 1);
    CHECK(u.done == 1);
    CHECK(read_file(fx / "dst/a.txt") == "edited by the user"); // left alone
    CHECK_FALSE(present(fx / "dst/b.txt"));
    REQUIRE(u.record);
    CHECK(record_of(u).steps.size() == 1); // only the undone step goes to redo

    // A folder Navigator made that is no longer empty is left alone.
    fx.dir("p");
    Trip t2(env);
    t2.forward(mkdir_req(fx / "p", "made"));
    fx.file("p/made/user-file", "u");
    const OpResult u2 = t2.undo();
    CHECK(u2.changed == 1);
    CHECK(read_file(fx / "p/made/user-file") == "u");

    // A vanished item is Changed too.
    fx.file("src/c.txt", "c");
    Trip t3(env);
    t3.forward(move_req({fx / "src/c.txt"}, fx / "dst"));
    std::filesystem::rename(fx / "dst/c.txt", fx / "dst/c-renamed.txt");
    CHECK(t3.undo().changed == 1);
}

TEST_CASE("permanent delete produces no undo record", "[undo][delete]") {
    Fixture fx;
    Env env(fx);
    fx.file("x", "x");
    auto r = run(env, delete_req({fx / "x"}));
    REQUIRE(r);
    CHECK(r->done == 1);
    CHECK_FALSE(r->record);
    OpRequest bad;
    bad.kind = OpKind::Undo;
    UndoRecord rec;
    rec.kind = OpKind::DeletePermanently;
    bad.record = std::make_shared<const UndoRecord>(rec);
    CHECK(run(env, bad).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("cancelled copy keeps completed items in the record", "[undo][cancel]") {
    Fixture fx;
    Env env(fx);
    fx.file("src/a", "a");
    fx.file("src/b", "b");
    fx.file("src/c", "c");
    std::stop_source stop;
    int seen = 0;
    auto r = run(env, copy_req({fx / "src/a", fx / "src/b", fx / "src/c"}, fx.dir("dst")), {},
                 stop.get_token(), [&](const Path&) {
                     if (++seen == 2) {
                         stop.request_stop(); // cancelled while placing b
                     }
                     return true;
                 });
    REQUIRE(r);
    CHECK(r->cancelled);
    CHECK(r->items[0].status == ItemStatus::Done);
    CHECK(r->items[2].status == ItemStatus::Cancelled);
    REQUIRE(r->record);
    CHECK(record_of(*r).steps.front().after.path == fx / "dst/a");
    CHECK_FALSE(present(fx / "dst/c"));
    CHECK(part_files(fx.root()).empty());
}
