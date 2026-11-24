// Phase 7 focus scoping in text fields: Shift+Delete, Ctrl+Shift+Z and Ctrl+Y are the field's,
// never file commands; the rename editor selects the stem; the filter field consumes the same
// keys (docs/keymap.md "Focus scoping (Phase 7)").
#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>

#include "search/filter_rules.hpp"
#include "widgets/text_edit.hpp"

using namespace navigator;
using namespace navigator::ui;

namespace {
KeyChord chord(std::string_view s) {
    auto c = parse_key_chord(s);
    REQUIRE(c);
    return *c;
}
} // namespace

TEST_CASE("Text fields consume the keys that would act on files", "[fields][phase7]") {
    TextEdit e;
    e.set("hello");
    e.home(false);
    // Shift+Delete deletes forward like Delete, and is handled (never file.delete_permanently).
    FieldKeyResult r = apply_field_key(e, chord("Shift+Delete"));
    CHECK(r.handled);
    CHECK(r.changed);
    CHECK(e.text() == "ello");
    // Ctrl+Shift+Z and Ctrl+Y are the field's text redo: handled, harmless.
    r = apply_field_key(e, chord("Ctrl+Shift+Z"));
    CHECK(r.handled);
    CHECK_FALSE(r.changed);
    r = apply_field_key(e, chord("Ctrl+Y"));
    CHECK(r.handled);
    CHECK_FALSE(r.changed);
    // The Phase 4 ones still work.
    r = apply_field_key(e, chord("Ctrl+Z"));
    CHECK(r.handled);
    CHECK(e.text() == "hello");
    CHECK(apply_field_key(e, chord("Delete")).handled);
    CHECK(apply_field_key(e, chord("Ctrl+C")).handled);
    CHECK(apply_field_key(e, chord("Ctrl+X")).handled);
    CHECK(apply_field_key(e, chord("Ctrl+V"), [] { return std::string("v"); }).handled);
    // F2 and Ctrl+Shift+N are not editing keys: they stay with the keymap (a field is not a
    // file selection, and the App does not route file commands while a field has focus).
    CHECK_FALSE(apply_field_key(e, chord("F2")).handled);
    CHECK_FALSE(apply_field_key(e, chord("Ctrl+Shift+N")).handled);
    // Alt chords stay commands.
    CHECK_FALSE(apply_field_key(e, chord("Alt+Left")).handled);
}

TEST_CASE("The filter field keeps Delete, Shift+Delete and the redo keys", "[fields][phase7]") {
    CHECK(route_filter_key(chord("Delete"), false) == FilterKey::Edit);
    CHECK(route_filter_key(chord("Shift+Delete"), false) == FilterKey::Edit);
    CHECK(route_filter_key(chord("Ctrl+Z"), false) == FilterKey::Edit);
    CHECK(route_filter_key(chord("Ctrl+Shift+Z"), false) == FilterKey::Edit);
    CHECK(route_filter_key(chord("Ctrl+Y"), false) == FilterKey::Edit);
    CHECK(route_filter_key(chord("Ctrl+V"), false) == FilterKey::Edit);
    // Ctrl+C / Ctrl+X are the field's only with a text selection, else they copy / cut files.
    CHECK(route_filter_key(chord("Ctrl+C"), false) == FilterKey::Keymap);
    CHECK(route_filter_key(chord("Ctrl+C"), true) == FilterKey::Edit);
    CHECK(route_filter_key(chord("Ctrl+X"), false) == FilterKey::Keymap);
    // File commands that are not editing keys reach the pane.
    CHECK(route_filter_key(chord("F2"), false) == FilterKey::Keymap);
    CHECK(route_filter_key(chord("Ctrl+Shift+N"), false) == FilterKey::Keymap);
    CHECK(field_consumes_as_text(chord("Space")));
}

TEST_CASE("select_range selects a byte range on code point boundaries", "[fields]") {
    TextEdit e;
    e.set("şarkı.txt");
    e.select_range(0, std::string("şarkı").size());
    CHECK(e.selection() == std::pair<std::size_t, std::size_t>{0, 7});
    CHECK(e.selected_text() == "şarkı");
    // A range that cuts a two-byte letter moves back to its start.
    e.select_range(0, 1);
    CHECK(e.selection().second == 0);
    e.select_range(3, 99);
    CHECK(e.selection().second == e.text().size());
    // Typing replaces the selection: the extension stays.
    e.select_range(0, 7);
    e.insert("yeni");
    CHECK(e.text() == "yeni.txt");
}
