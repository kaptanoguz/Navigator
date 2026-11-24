// ui::FileOps (Phase 7): the controller between commands and the operation queue, on fixture
// directories with a fixture trash. Every test creates and removes files only inside its
// TempDir; nothing touches $HOME or the real trash.
#include "file_ops_test.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>
#include <navigator/ops.hpp>
#include <navigator/transfer.hpp>

#include "i18n/catalog.hpp"
#include "ops/app_trash.hpp"
#include "ops/op_text.hpp"
#include "trash/trash_model.hpp"

using namespace navigator;
using namespace navigator::ui;
using navigator::test::OpsRig;

namespace {

FileTarget target_of(const OpsRig& r, std::string_view dir,
                     std::initializer_list<const char*> names) {
    FileTarget t;
    t.dir = r.p(dir);
    for (const char* n : names) {
        t.selected.push_back(r.p(dir) / n);
    }
    return t;
}

struct EnglishScope {
    EnglishScope() { set_language(Language::English); }
    ~EnglishScope() { set_language(Language::English); }
};

} // namespace

TEST_CASE("Copy and paste put the transfer formats on the clipboard and copy the files",
          "[ops][clipboard]") {
    const EnglishScope en;
    OpsRig r;
    r.t.file("A/a.txt", "alpha");
    r.t.file("A/b.txt", "beta");
    r.t.dir("B");
    r.ops->copy(target_of(r, "A", {"a.txt", "b.txt"}), false);
    // The formats of transfer::make_clipboard_offer, GNOME and plain.
    REQUIRE(r.clip.owns_offer());
    CHECK(r.clip.data().contains(std::string(transfer::kMimeGnomeCopiedFiles)));
    CHECK(r.clip.data().contains(std::string(transfer::kMimeUriList)));
    CHECK(r.clip.data().contains(std::string(transfer::kMimeText)));
    CHECK_FALSE(r.clip.data().contains(std::string(transfer::kMimeKdeCutSelection)));
    CHECK(r.clip.data().at(std::string(transfer::kMimeGnomeCopiedFiles)).starts_with("copy\n"));
    CHECK_FALSE(r.ops->has_cut());

    FileTarget dest;
    dest.dir = r.p("B");
    r.ops->paste(dest, false);
    REQUIRE(r.settle());
    CHECK(OpsRig::read(r.p("B/a.txt")) == "alpha");
    CHECK(OpsRig::read(r.p("B/b.txt")) == "beta");
    CHECK(OpsRig::exists(r.p("A/a.txt"))); // a copy keeps the originals
    // The new items are selected once they appear.
    const Reconcile rc = r.ops->take_reconcile();
    REQUIRE_FALSE(rc.select.empty());
    CHECK(rc.select.front().dir == r.p("B"));
    CHECK(rc.select.front().names.size() == 2);
}

TEST_CASE("Cut dims rows, paste moves, the clipboard is cleared, undo and redo round trip",
          "[ops][undo]") {
    const EnglishScope en;
    OpsRig r;
    r.t.file("A/a.txt", "alpha");
    r.t.dir("B");
    r.ops->copy(target_of(r, "A", {"a.txt"}), true);
    CHECK(r.clip.data().contains(std::string(transfer::kMimeKdeCutSelection)));
    CHECK(r.ops->is_cut(r.p("A/a.txt")));
    CHECK_FALSE(r.ops->is_cut(r.p("A/other")));

    FileTarget dest;
    dest.dir = r.p("B");
    r.ops->paste(dest, false);
    REQUIRE(r.settle());
    CHECK_FALSE(OpsRig::exists(r.p("A/a.txt")));
    CHECK(OpsRig::exists(r.p("B/a.txt")));
    CHECK_FALSE(r.ops->has_cut()); // cleared after the paste
    CHECK_FALSE(r.clip.owns_offer());
    REQUIRE(r.ops->undo_text().has_value());
    CHECK(*r.ops->undo_text() == "move “a.txt” to B");

    // Acceptance: a move between two folders comes back with Undo.
    r.ops->undo();
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A/a.txt")));
    CHECK_FALSE(OpsRig::exists(r.p("B/a.txt")));
    REQUIRE(r.ops->notice().has_value());
    CHECK(r.ops->notice()->text.starts_with("Undone: "));
    CHECK(r.ops->notice()->action == NoticeAction::Redo);
    CHECK(r.ops->redo_text().has_value());

    r.ops->redo();
    REQUIRE(r.settle());
    CHECK_FALSE(OpsRig::exists(r.p("A/a.txt")));
    CHECK(OpsRig::exists(r.p("B/a.txt")));
    // Undo and redo with nothing to do say so.
    r.ops->undo();
    REQUIRE(r.settle());
    r.ops->undo();
    REQUIRE(r.settle());
    CHECK(r.ops->notice()->text == "Nothing to undo");
}

TEST_CASE("Paste reads Nautilus and Dolphin clipboards and ignores plain text",
          "[ops][clipboard]") {
    const EnglishScope en;
    OpsRig r;
    r.t.file("A/n.txt", "nautilus");
    r.t.file("A/d.txt", "dolphin");
    r.t.dir("B");
    FileTarget dest;
    dest.dir = r.p("B");
    const std::string uri_n = "file://" + r.p("A/n.txt").string();
    const std::string uri_d = "file://" + r.p("A/d.txt").string();

    r.clip.set_foreign({{"x-special/gnome-copied-files", "cut\n" + uri_n}});
    r.ops->paste(dest, false);
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("B/n.txt")));
    CHECK_FALSE(OpsRig::exists(r.p("A/n.txt"))); // "cut" moves, even from another program

    r.clip.set_foreign(
        {{"text/uri-list", uri_d + "\r\n"}, {"application/x-kde-cutselection", "1"}});
    r.ops->paste(dest, false);
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("B/d.txt")));
    CHECK_FALSE(OpsRig::exists(r.p("A/d.txt")));

    // A path as plain text is text, never an implicit copy.
    r.t.file("A/p.txt", "plain");
    r.clip.set_foreign({{"text/plain", r.p("A/p.txt").string()}});
    r.ops->paste(dest, false);
    REQUIRE(r.settle());
    CHECK_FALSE(OpsRig::exists(r.p("B/p.txt")));
    REQUIRE(r.ops->notice().has_value());
    CHECK(r.ops->notice()->text == "The clipboard has no files to paste");
}

TEST_CASE("Paste into the cursor folder", "[ops][clipboard]") {
    OpsRig r;
    r.t.file("A/a.txt", "a");
    r.t.dir("B/inner");
    r.ops->copy(target_of(r, "A", {"a.txt"}), false);
    FileTarget dest;
    dest.dir = r.p("B");
    dest.cursor = r.p("B/inner");
    dest.cursor_is_dir = true;
    r.ops->paste(dest, true);
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("B/inner/a.txt")));
    CHECK_FALSE(OpsRig::exists(r.p("B/a.txt")));
}

TEST_CASE("Delete means trash: the notice offers Undo and the Trash view restores",
          "[ops][trash]") {
    const EnglishScope en;
    OpsRig r;
    r.t.file("A/a.txt", "alpha");
    r.t.file("A/b.txt", "beta");
    r.ops->trash(target_of(r, "A", {"a.txt", "b.txt"}));
    REQUIRE(r.settle());
    CHECK_FALSE(OpsRig::exists(r.p("A/a.txt")));
    REQUIRE(r.ops->notice().has_value());
    CHECK(r.ops->notice()->text == "Moved 2 items to the trash");
    CHECK(r.ops->notice()->action == NoticeAction::Undo); // "Moved 2 items to the trash.  Undo"
    CHECK_FALSE(r.ops->notice()->error);
    CHECK_FALSE(r.ops->sheet_open()); // no confirmation for the trash

    // The Trash view lists both, newest first, with their original locations.
    TrashModel::Options to;
    to.trash = r.trash;
    TrashModel model(std::move(to));
    model.refresh();
    REQUIRE(model.wait_listing(std::chrono::seconds(5)));
    REQUIRE(model.count() == 2);
    CHECK(model.items()[0].original_path.parent_path() == r.p("A"));
    // Restore one item (acceptance: a trashed item is restored from the Trash view).
    model.click_row(0, Modifiers::None);
    const std::string restored_name = TrashModel::display_name_of(model.items()[0]);
    r.ops->restore(model.selected_or_cursor());
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A") / restored_name));
    model.refresh();
    REQUIRE(model.wait_listing(std::chrono::seconds(5)));
    CHECK(model.count() == 1);
}

TEST_CASE("Undo of a trash brings the item back", "[ops][trash][undo]") {
    OpsRig r;
    r.t.file("A/a.txt", "alpha");
    r.ops->trash(target_of(r, "A", {"a.txt"}));
    REQUIRE(r.settle());
    REQUIRE_FALSE(OpsRig::exists(r.p("A/a.txt")));
    r.ops->undo();
    REQUIRE(r.settle());
    CHECK(OpsRig::read(r.p("A/a.txt")) == "alpha");
}

TEST_CASE("Permanent delete is gated by a confirmation sheet that starts on Cancel",
          "[ops][safety]") {
    const EnglishScope en;
    OpsRig r;
    r.t.file("A/a.txt", "alpha");
    r.t.file("A/b.txt", "beta");
    r.ops->request_delete_permanently(target_of(r, "A", {"a.txt", "b.txt"}));
    REQUIRE(r.ops->sheet_kind() == SheetKind::DeletePermanently);
    const ConfirmSheet* s = r.ops->confirm_sheet();
    REQUIRE(s != nullptr);
    CHECK(s->focus == 0); // Cancel is the default
    CHECK(s->count == 2);
    // Enter on the default button cancels; nothing was deleted.
    r.ops->sheet_activate();
    CHECK_FALSE(r.ops->sheet_open());
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A/a.txt")));
    // Escape cancels too.
    r.ops->request_delete_permanently(target_of(r, "A", {"a.txt"}));
    r.ops->sheet_escape();
    CHECK_FALSE(r.ops->sheet_open());
    CHECK(OpsRig::exists(r.p("A/a.txt")));
    // Tab moves to the destructive button; Enter then deletes, for good (no undo entry).
    r.ops->request_delete_permanently(target_of(r, "A", {"a.txt", "b.txt"}));
    r.ops->sheet_focus(1);
    REQUIRE(r.ops->confirm_sheet()->focus == 1);
    r.ops->sheet_activate();
    REQUIRE(r.settle());
    CHECK_FALSE(OpsRig::exists(r.p("A/a.txt")));
    CHECK_FALSE(OpsRig::exists(r.p("A/b.txt")));
    CHECK_FALSE(r.ops->undo_text().has_value());
    REQUIRE(r.ops->notice().has_value());
    CHECK(r.ops->notice()->text == "Deleted 2 items permanently");
    CHECK(r.ops->notice()->action == NoticeAction::None);
}

TEST_CASE("The sheet counts a folder tree off the UI thread", "[ops][safety]") {
    OpsRig r;
    r.t.file("A/d/x.bin", std::string(1000, 'x'));
    r.t.file("A/d/sub/y.bin", std::string(500, 'y'));
    FileTarget t;
    t.dir = r.p("A");
    t.selected = {r.p("A/d")};
    r.ops->request_delete_permanently(t);
    std::optional<TreeStats> st;
    for (int i = 0; i < 400; ++i) {
        st = r.ops->confirm_stats();
        if (st && st->done) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(st.has_value());
    CHECK(st->done);
    CHECK(st->files == 2);
    CHECK(st->folders == 2);
    CHECK(st->bytes == 1500);
    r.ops->sheet_escape();
    CHECK(OpsRig::exists(r.p("A/d/x.bin")));
}

TEST_CASE("Without a trash nothing is deleted: the untrashable sheet asks",
          "[ops][safety][trash]") {
    const EnglishScope en;
    OpsRig r(false); // no ITrash: the headless rule
    r.t.file("A/a.txt", "alpha");
    r.ops->trash(target_of(r, "A", {"a.txt"}));
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A/a.txt"))); // S2: never a permanent fallback
    REQUIRE(r.ops->sheet_kind() == SheetKind::DeleteUntrashable);
    CHECK(r.ops->confirm_sheet()->focus == 0);
    REQUIRE(r.ops->notice().has_value());
    CHECK(r.ops->notice()->error);
    CHECK(r.ops->notice()->text.find("no usable trash") != std::string::npos);
    r.ops->sheet_escape();
    CHECK(OpsRig::exists(r.p("A/a.txt")));
    // Explicit, confirmed permanent delete works for exactly those items.
    r.ops->trash(target_of(r, "A", {"a.txt"}));
    REQUIRE(r.settle());
    r.ops->sheet_focus(1);
    r.ops->sheet_activate();
    REQUIRE(r.settle());
    CHECK_FALSE(OpsRig::exists(r.p("A/a.txt")));
}

TEST_CASE("Headless runs have no trash", "[ops][safety][trash]") {
    CHECK(trash_source_for("", true) == TrashSource::None);
    CHECK(trash_source_for("/tmp/x", true) == TrashSource::Injected);
    CHECK(trash_source_for("", false) == TrashSource::System);
    const AppTrash none = create_app_trash("", true);
    CHECK(none.trash == nullptr);
    CHECK_FALSE(none.note.empty());
    // Restoring and emptying fail with Unsupported through the model.
    TrashModel::Options to;
    to.trash = none.trash;
    TrashModel model(std::move(to));
    CHECK_FALSE(model.available());
    model.refresh();
    REQUIRE(model.error().has_value());
    CHECK(model.error()->code == ErrorCode::Unsupported);
    // An injected trash works and lives only in its directory.
    test::TempDir t;
    const AppTrash fixture = create_app_trash((t.path() / "tr").string(), true);
    REQUIRE(fixture.trash != nullptr);
    CHECK(fixture.source == TrashSource::Injected);
}

TEST_CASE("Conflicts: Enter keeps both by default, Escape cancels the operation",
          "[ops][conflict]") {
    const EnglishScope en;
    OpsRig r;
    r.t.file("A/a.txt", "incoming");
    r.t.file("B/a.txt", "existing");
    r.ops->copy(target_of(r, "A", {"a.txt"}), false);
    FileTarget dest;
    dest.dir = r.p("B");
    r.ops->paste(dest, false);
    for (int i = 0; i < 400 && r.ops->sheet_kind() != SheetKind::Conflict; ++i) {
        r.ops->frame_start(r.now += 10.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(r.ops->sheet_kind() == SheetKind::Conflict);
    const ConflictSheet* s = r.ops->conflict_sheet();
    REQUIRE(s != nullptr);
    const std::vector<ConflictButton> buttons = conflict_buttons(s->conflict);
    CHECK(buttons[s->focus] == ConflictButton::KeepBoth); // Enter = Keep both
    CHECK(s->suggested == "a (2).txt");
    CHECK(s->name.text() == "a (2).txt");
    CHECK_FALSE(s->conflict.replace_is_permanent); // with a trash, Replace sends the old file there
    r.ops->sheet_activate();
    REQUIRE(r.settle());
    CHECK(OpsRig::read(r.p("B/a.txt")) == "existing");
    CHECK(OpsRig::read(r.p("B/a (2).txt")) == "incoming");

    // Escape = Cancel operation: nothing more happens.
    r.t.file("A/b.txt", "b1");
    r.t.file("B/b.txt", "b2");
    r.ops->copy(target_of(r, "A", {"b.txt"}), false);
    r.ops->paste(dest, false);
    for (int i = 0; i < 400 && r.ops->sheet_kind() != SheetKind::Conflict; ++i) {
        r.ops->frame_start(r.now += 10.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(r.ops->sheet_kind() == SheetKind::Conflict);
    r.ops->sheet_escape();
    REQUIRE(r.settle());
    CHECK(OpsRig::read(r.p("B/b.txt")) == "b2");
    CHECK_FALSE(OpsRig::exists(r.p("B/b (2).txt")));
    CHECK(r.ops->history().back().cancelled);
}

TEST_CASE("Conflicts: Replace goes through the trash, Skip leaves both, a typed name is validated",
          "[ops][conflict]") {
    const EnglishScope en;
    OpsRig r;
    r.t.file("A/a.txt", "incoming");
    r.t.file("A/c.txt", "c-in");
    r.t.file("B/a.txt", "existing");
    r.t.file("B/c.txt", "c-existing");
    FileTarget dest;
    dest.dir = r.p("B");
    const auto wait_sheet = [&] {
        for (int i = 0; i < 400 && r.ops->sheet_kind() != SheetKind::Conflict; ++i) {
            r.ops->frame_start(r.now += 10.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return r.ops->sheet_kind() == SheetKind::Conflict;
    };
    // Replace: the old file is in the trash, the new one in place.
    r.ops->copy(target_of(r, "A", {"a.txt"}), false);
    r.ops->paste(dest, false);
    REQUIRE(wait_sheet());
    const std::vector<ConflictButton> b = conflict_buttons(r.ops->conflict_sheet()->conflict);
    const auto replace_at = std::ranges::find(b, ConflictButton::Replace) - b.begin();
    r.ops->sheet_activate(static_cast<std::size_t>(replace_at));
    REQUIRE(r.settle());
    CHECK(OpsRig::read(r.p("B/a.txt")) == "incoming");
    TrashModel::Options to;
    to.trash = r.trash;
    TrashModel model(std::move(to));
    model.refresh();
    REQUIRE(model.wait_listing(std::chrono::seconds(5)));
    CHECK(model.count() == 1);

    // Skip.
    r.ops->copy(target_of(r, "A", {"c.txt"}), false);
    r.ops->paste(dest, false);
    REQUIRE(wait_sheet());
    {
        const std::vector<ConflictButton> sb = conflict_buttons(r.ops->conflict_sheet()->conflict);
        r.ops->sheet_activate(
            static_cast<std::size_t>(std::ranges::find(sb, ConflictButton::Skip) - sb.begin()));
    }
    REQUIRE(r.settle());
    CHECK(OpsRig::read(r.p("B/c.txt")) == "c-existing");
    CHECK(r.ops->notice()->text == "Nothing to do: the items are already there");

    // A typed name: invalid names keep the sheet open with a message, a valid one is used.
    r.ops->copy(target_of(r, "A", {"c.txt"}), false);
    r.ops->paste(dest, false);
    REQUIRE(wait_sheet());
    r.ops->conflict_begin_name_edit();
    ConflictSheet* cs = r.ops->conflict_sheet();
    REQUIRE(cs != nullptr);
    cs->name.set("bad/name");
    r.ops->conflict_name_edited();
    CHECK_FALSE(cs->name_error.empty());
    r.ops->sheet_activate(); // Enter while editing = Keep both with that name
    REQUIRE(r.ops->sheet_kind() == SheetKind::Conflict); // still open
    cs = r.ops->conflict_sheet();
    cs->name.set("şarkı (kopya).txt");
    r.ops->conflict_name_edited();
    CHECK(cs->name_error.empty());
    r.ops->sheet_activate();
    REQUIRE(r.settle());
    CHECK(OpsRig::read(r.p("B/şarkı (kopya).txt")) == "c-in");
}

TEST_CASE("Apply to all answers the remaining conflicts", "[ops][conflict]") {
    OpsRig r;
    for (const char* n : {"a.txt", "b.txt", "c.txt"}) {
        r.t.file(std::string("A/") + n, "in");
        r.t.file(std::string("B/") + n, "old");
    }
    r.ops->copy(target_of(r, "A", {"a.txt", "b.txt", "c.txt"}), false);
    FileTarget dest;
    dest.dir = r.p("B");
    r.ops->paste(dest, false);
    for (int i = 0; i < 400 && r.ops->sheet_kind() != SheetKind::Conflict; ++i) {
        r.ops->frame_start(r.now += 10.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(r.ops->sheet_kind() == SheetKind::Conflict);
    r.ops->sheet_toggle_apply_all();
    CHECK(r.ops->conflict_sheet()->apply_all);
    r.ops->sheet_activate(); // Keep both for all three
    REQUIRE(r.settle());
    for (const char* n : {"a (2).txt", "b (2).txt", "c (2).txt"}) {
        CHECK(OpsRig::exists(r.p("B") / n));
    }
    CHECK(OpsRig::read(r.p("B/a.txt")) == "old");
}

TEST_CASE("Inline rename: stem selection, validation and Turkish names", "[ops][rename]") {
    const EnglishScope en;
    OpsRig r;
    r.t.file("A/şarkı sözleri.txt", "x");
    r.t.file("A/archive.tar.gz", "x");
    r.t.file("A/other.txt", "x");
    r.t.dir("A/Klasör");
    const auto taken = [&](std::string_view n) {
        return OpsRig::exists(r.p("A") / std::string(n));
    };

    // The stem is selected, the extension stays.
    r.ops->begin_rename(layout::TabId{1}, r.p("A"), "archive.tar.gz", false);
    InlineEdit* ie = r.ops->inline_edit();
    REQUIRE(ie != nullptr);
    CHECK(ie->edit.selection() == std::pair<std::size_t, std::size_t>{0, 7}); // "archive"
    ie->edit.insert("backup");
    r.ops->inline_edited(taken);
    CHECK(ie->message.empty());
    CHECK(ie->edit.text() == "backup.tar.gz");
    // A folder selects its whole name.
    r.ops->begin_rename(layout::TabId{1}, r.p("A"), "Klasör", true);
    CHECK(r.ops->inline_edit()->edit.selection().second == std::string("Klasör").size());

    // Turkish file name: the stem selection is in bytes of "şarkı sözleri".
    r.ops->begin_rename(layout::TabId{1}, r.p("A"), "şarkı sözleri.txt", false);
    ie = r.ops->inline_edit();
    CHECK(ie->edit.selection().second == std::string("şarkı sözleri").size());
    ie->edit.insert("İstanbul çiçeği");
    r.ops->inline_edited(taken);
    CHECK(ie->message.empty());

    // Validation.
    ie->edit.set("");
    r.ops->inline_edited(taken);
    CHECK(ie->message_error);
    CHECK(ie->message == "A name cannot be empty");
    ie->edit.set("a/b");
    r.ops->inline_edited(taken);
    CHECK(ie->message_error);
    ie->edit.set("..");
    r.ops->inline_edited(taken);
    CHECK(ie->message_error);
    ie->edit.set("other.txt");
    r.ops->inline_edited(taken);
    CHECK(ie->message_error);
    CHECK(ie->message == "“other.txt” already exists in this folder");
    // The field refuses to grow past NAME_MAX bytes; Turkish letters are 2 bytes each, so 130 of
    // them stop at 127 (254 bytes), on a code point boundary.
    ie->edit.set("");
    for (int i = 0; i < 130; ++i) {
        ie->edit.insert("ğ");
    }
    r.ops->inline_edited(taken);
    CHECK(ie->edit.text().size() <= ops::kNameMaxBytes);
    CHECK(ie->edit.text().size() % 2 == 0);
    CHECK_FALSE(ie->message_error);
    // Warnings do not refuse.
    ie->edit.set(".hidden");
    r.ops->inline_edited(taken);
    CHECK_FALSE(ie->message_error);
    CHECK(ie->message == "A name starting with a dot hides the item");
    ie->edit.set("trailing ");
    r.ops->inline_edited(taken);
    CHECK_FALSE(ie->message_error);
    CHECK_FALSE(ie->message.empty());

    // An invalid name keeps the editor open on commit; a valid one renames.
    ie->edit.set("other.txt");
    CHECK_FALSE(r.ops->inline_commit(taken));
    REQUIRE(r.ops->inline_edit() != nullptr);
    r.ops->inline_edit()->edit.set("İstanbul çiçeği.txt");
    CHECK(r.ops->inline_commit(taken));
    CHECK(r.ops->inline_edit() == nullptr);
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A/İstanbul çiçeği.txt")));
    CHECK_FALSE(OpsRig::exists(r.p("A/şarkı sözleri.txt")));
    // Undo renames back.
    r.ops->undo();
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A/şarkı sözleri.txt")));
    // Escape: nothing changes.
    r.ops->begin_rename(layout::TabId{1}, r.p("A"), "other.txt", false);
    r.ops->inline_edit()->edit.set("changed.txt");
    r.ops->inline_cancel();
    CHECK(r.ops->inline_edit() == nullptr);
    CHECK(OpsRig::exists(r.p("A/other.txt")));
    // Unchanged text commits without an operation.
    r.ops->begin_rename(layout::TabId{1}, r.p("A"), "other.txt", false);
    CHECK(r.ops->inline_commit(taken));
    CHECK_FALSE(r.ops->busy());
}

TEST_CASE("New folder gets a unique name and asks for the inline editor", "[ops][rename]") {
    OpsRig r;
    r.t.dir("A/New folder");
    FileTarget t;
    t.tab = layout::TabId{7};
    t.dir = r.p("A");
    r.ops->new_folder(t);
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A/New folder (2)")));
    const Reconcile rc = r.ops->take_reconcile();
    REQUIRE(rc.rename_after_create.has_value());
    CHECK(rc.rename_after_create->name == "New folder (2)");
    CHECK(rc.rename_after_create->tab == layout::TabId{7});
    // Undo removes the empty folder it created, and only that.
    r.ops->undo();
    REQUIRE(r.settle());
    CHECK_FALSE(OpsRig::exists(r.p("A/New folder (2)")));
    CHECK(OpsRig::exists(r.p("A/New folder")));
}

TEST_CASE("Partial failures are reported with the failing item", "[ops][errors]") {
    const EnglishScope en;
    OpsRig r(true, [](const Path& p) { return p.filename() != "b.txt"; }); // b.txt fails (injected)
    for (const char* n : {"a.txt", "b.txt", "c.txt"}) {
        r.t.file(std::string("A/") + n, "x");
    }
    r.t.dir("B");
    r.ops->copy_to_folder(target_of(r, "A", {"a.txt", "b.txt", "c.txt"}), r.p("B"), false);
    REQUIRE(r.settle());
    REQUIRE_FALSE(r.ops->history().empty());
    const FinishedOp& f = r.ops->history().back();
    CHECK_FALSE(f.ok);
    CHECK(f.title == "Copied 2 of 3 items");
    REQUIRE_FALSE(f.details.empty());
    CHECK(f.details.front().find("b.txt") != std::string::npos);
    REQUIRE(r.ops->notice().has_value());
    CHECK(r.ops->notice()->error);
    CHECK(r.ops->notice()->action == NoticeAction::ShowPanel);
}

TEST_CASE("Error sentences are plain language in English and Turkish", "[ops][errors][i18n]") {
    const std::vector<ErrorCode> codes{ErrorCode::NoSpace,          ErrorCode::ReadOnlyFilesystem,
                                       ErrorCode::PermissionDenied, ErrorCode::CrossDevice,
                                       ErrorCode::NotFound,         ErrorCode::IoError,
                                       ErrorCode::NotEmpty,         ErrorCode::Unsupported};
    for (const Language lang : {Language::English, Language::Turkish}) {
        set_language(lang);
        for (const ErrorCode c : codes) {
            const std::string text = error_sentence(c, "a.txt");
            INFO(to_string(c));
            CHECK(text.find("a.txt") != std::string::npos);
            CHECK(text.find('{') == std::string::npos);
        }
    }
    set_language(Language::English);
    CHECK(error_sentence(ErrorCode::NoSpace, "a.txt") ==
          "Not enough space on the destination for “a.txt”.");
    set_language(Language::Turkish);
    CHECK(error_sentence(ErrorCode::NoSpace, "a.txt") ==
          "“a.txt” için hedefte yeterli boş alan yok.");
    CHECK(changed_sentence("x").find("sonradan") != std::string::npos);
    set_language(Language::English);
    CHECK(changed_sentence("x") == "“x” changed since, so it was left as it is.");
}

TEST_CASE("Op titles and summaries handle plurals in both languages", "[ops][i18n]") {
    set_language(Language::English);
    CHECK(op_title(ops::OpKind::Copy, 1, "a.txt", "Documents") == "Copying “a.txt” to Documents");
    CHECK(op_title(ops::OpKind::Copy, 3, "a.txt", "Documents") == "Copying 3 items to Documents");
    CHECK(op_done_text(ops::OpKind::Trash, 3, "a", "") == "Moved 3 items to the trash");
    CHECK(op_partial_text(ops::OpKind::Move, 2, 3, false) == "Moved 2 of 3 items");
    CHECK(op_partial_text(ops::OpKind::Move, 1, 3, true) == "Cancelled: 1 of 3 items done");
    CHECK(op_title(ops::OpKind::Copy, 1500, "", "X") == "Copying 1,500 items to X");
    set_language(Language::Turkish);
    CHECK(op_title(ops::OpKind::Copy, 3, "a.txt", "Belgeler") ==
          "3 öğe Belgeler klasörüne kopyalanıyor");
    CHECK(op_title(ops::OpKind::Copy, 1, "a.txt", "Belgeler") ==
          "1 öğe Belgeler klasörüne kopyalanıyor");
    CHECK(op_done_text(ops::OpKind::Trash, 3, "a", "") == "3 öğe çöp kutusuna taşındı");
    CHECK(op_partial_text(ops::OpKind::Move, 2, 3, false) == "3 öğeden 2 tanesi taşındı");
    set_language(Language::English);
    CHECK(format_eta(std::chrono::seconds(45)) == "45 s left");
    CHECK(format_eta(std::chrono::seconds(80)) == "1 min 20 s left");
    CHECK(format_eta(std::chrono::seconds(7500)) == "2 h 5 min left");
    CHECK(format_rate(12.4e6).ends_with("/s"));
    ops::OpProgress p;
    p.phase = ops::OpPhase::Scanning;
    CHECK_FALSE(op_fraction(p).has_value());
    p.phase = ops::OpPhase::Working;
    p.bytes_total = 200;
    p.bytes_done = 50;
    p.totals_final = true;
    REQUIRE(op_fraction(p).has_value());
    CHECK(*op_fraction(p) == 0.25);
}

TEST_CASE("Quit while operations run asks; the answers are keep, wait or cancel", "[ops][quit]") {
    std::atomic<bool> release{false};
    OpsRig r(true, [&release](const Path&) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    });
    // Nothing runs: quit at once.
    CHECK(r.ops->request_quit());
    CHECK(r.ops->quit_state() == QuitState::Quit);

    OpsRig q(true, [&release](const Path&) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    });
    q.t.file("A/a.txt", "x");
    q.t.dir("B");
    q.ops->copy_to_folder(target_of(q, "A", {"a.txt"}), q.p("B"), false);
    for (int i = 0; i < 200 && !q.ops->busy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(q.ops->busy());
    CHECK_FALSE(q.ops->request_quit());
    CHECK(q.ops->sheet_kind() == SheetKind::QuitRunning);
    CHECK(q.ops->quit_focus() == 0); // "Keep working" is the default
    q.ops->sheet_activate();
    CHECK(q.ops->quit_state() == QuitState::Running);
    CHECK(q.ops->busy());
    // Wait, then quit.
    CHECK_FALSE(q.ops->request_quit());
    q.ops->sheet_focus(1);
    q.ops->sheet_activate();
    CHECK(q.ops->quit_state() == QuitState::WaitingToQuit);
    release = true;
    REQUIRE(q.settle());
    CHECK(q.ops->quit_state() == QuitState::Quit);
    CHECK(OpsRig::exists(q.p("B/a.txt")));
}

TEST_CASE("Cancel and quit stops the operation safely", "[ops][quit]") {
    std::atomic<bool> release{false};
    OpsRig q(true, [&release](const Path&) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    });
    q.t.file("A/a.txt", "x");
    q.t.file("A/b.txt", "y");
    q.t.dir("B");
    q.ops->copy_to_folder(target_of(q, "A", {"a.txt", "b.txt"}), q.p("B"), false);
    for (int i = 0; i < 200 && !q.ops->busy(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_FALSE(q.ops->request_quit());
    q.ops->sheet_activate(2); // Cancel and quit
    CHECK(q.ops->quit_state() == QuitState::Quit);
    release = true;
    q.ops->shutdown();
    // No truncated destination is left behind (S6, S9): either absent or complete.
    for (const char* n : {"B/a.txt", "B/b.txt"}) {
        if (OpsRig::exists(q.p(n))) {
            CHECK(OpsRig::read(q.p(n)).size() == 1);
        }
    }
    for (const auto& e : std::filesystem::directory_iterator(q.p("B"))) {
        CHECK(e.path().filename().string().find(".navigator-") == std::string::npos);
    }
}

TEST_CASE("Protected folders are refused with a plain message", "[ops][safety]") {
    const EnglishScope en;
    OpsRig r;
    r.t.dir("home");
    FileTarget t;
    t.dir = r.t.path();
    t.selected = {r.p("home")}; // the configured home directory
    r.ops->trash(t);
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("home")));
    REQUIRE(r.ops->notice().has_value());
    CHECK(r.ops->notice()->error);
    CHECK(r.ops->notice()->text.find("protected") != std::string::npos);
}

TEST_CASE("Copy to a folder links and the other-pane helpers use the same path", "[ops][dnd]") {
    OpsRig r;
    r.t.file("A/a.txt", "x");
    r.t.dir("B");
    r.ops->transfer({r.p("A/a.txt")}, r.p("B"), transfer::DropAction::Link);
    REQUIRE(r.settle());
    CHECK(std::filesystem::is_symlink(r.p("B/a.txt")));
    r.ops->transfer({r.p("A/a.txt")}, r.p("B"), transfer::DropAction::None); // nothing
    r.ops->transfer({r.p("A/a.txt")}, r.p("B"), transfer::DropAction::Ask);  // the App asks first
    REQUIRE(r.settle());
    r.ops->transfer({r.p("A/a.txt")}, r.p("B"), transfer::DropAction::Move);
    for (int i = 0; i < 400 && r.ops->sheet_kind() != SheetKind::Conflict; ++i) {
        r.ops->frame_start(r.now += 10.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(r.ops->sheet_kind() == SheetKind::Conflict); // the link occupies the name
    {
        const std::vector<ConflictButton> sb = conflict_buttons(r.ops->conflict_sheet()->conflict);
        r.ops->sheet_activate(
            static_cast<std::size_t>(std::ranges::find(sb, ConflictButton::Skip) - sb.begin()));
    }
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A/a.txt"))); // skipped: the source stays
}

TEST_CASE("Progress shows rate and ETA only when the engine provides them", "[ops][progress]") {
    std::atomic<bool> release{false};
    OpsRig q(true, [&release](const Path&) {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    });
    q.t.file("A/a.txt", std::string(4096, 'x'));
    q.t.dir("B");
    q.ops->copy_to_folder(target_of(q, "A", {"a.txt"}), q.p("B"), false);
    for (int i = 0; i < 300 && q.ops->running().empty(); ++i) {
        q.ops->frame_start(q.now += 10.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE_FALSE(q.ops->running().empty());
    const ops::OpProgress p = q.ops->running().front();
    CHECK(p.kind == ops::OpKind::Copy);
    CHECK_FALSE(p.eta.has_value()); // no stable rate yet
    CHECK(q.ops->title_of(p).starts_with("Copying"));
    q.ops->toggle_pause(p.id.value);
    release = true;
    q.ops->cancel(p.id.value);
    q.settle();
}

TEST_CASE("No file command fires while the inline editor is open (BUG-P7-1)",
          "[ops][rename][fields]") {
    for (const CommandId id :
         {CommandId::Rename, CommandId::NewFolder, CommandId::CopyToOtherPane,
          CommandId::MoveToOtherPane, CommandId::Copy, CommandId::Cut, CommandId::Paste,
          CommandId::Undo, CommandId::Redo, CommandId::MoveToTrash, CommandId::DeletePermanently,
          CommandId::OpenTrash, CommandId::RestoreFromTrash, CommandId::EmptyTrash}) {
        INFO(static_cast<int>(id));
        CHECK(file_command_blocked_while_editing(id));
    }
    CHECK_FALSE(file_command_blocked_while_editing(CommandId::ToggleOperationsPanel));
    CHECK_FALSE(file_command_blocked_while_editing(CommandId::CancelOperation));
    CHECK_FALSE(file_command_blocked_while_editing(CommandId::Activate));
}

TEST_CASE("F2 does not restart the editor and typed text survives", "[ops][rename]") {
    OpsRig r;
    r.t.file("A/gamma.txt", "x");
    const auto taken = [&](std::string_view n) {
        return OpsRig::exists(r.p("A") / std::string(n));
    };
    r.ops->begin_rename(layout::TabId{1}, r.p("A"), "gamma.txt", false);
    r.ops->inline_edit()->edit.insert("zzz");
    // The App drops the blocked command; the editor and its text are untouched.
    REQUIRE(file_command_blocked_while_editing(CommandId::Rename));
    CHECK(r.ops->inline_edit()->edit.text() == "zzz.txt");
    CHECK(r.ops->inline_commit(taken));
    REQUIRE(r.settle());
    CHECK(OpsRig::exists(r.p("A/zzz.txt")));
}
