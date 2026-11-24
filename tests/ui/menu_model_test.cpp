// Tests for navigator/menu.hpp (builders, Pins, JSON & file I/O, arrange M1-M3, filter M4-M5)
// and src/ui/input/shortcut_text.hpp.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>
#include <navigator/menu.hpp>

#include "input/shortcut_text.hpp"
#include "ui_fixture.hpp"

namespace navigator::menu {
namespace {

CommandRegistry builtins() {
    CommandRegistry reg;
    REQUIRE(register_builtin_commands(reg).has_value());
    return reg;
}

TEST_CASE("menu::Pins pin, unpin, move, JSON and file I/O", "[ui][menu]") {
    Pins pins;
    REQUIRE(pins.pin("edit.copy_path").has_value());
    REQUIRE(pins.pin("edit.copy_path#1").has_value());
    CHECK(pins.contains("edit.copy_path"));
    CHECK(pins.pin("edit.copy_path").error().code == ErrorCode::AlreadyExists);

    // Non-pinnable ids rejected.
    CHECK(pins.pin("").error().code == ErrorCode::InvalidArgument);
    CHECK(pins.pin("submenu.open_with").error().code == ErrorCode::InvalidArgument);
    CHECK(pins.pin("open_with:gedit.desktop").error().code == ErrorCode::InvalidArgument);
    CHECK(pins.pin("file.delete_permanently").error().code == ErrorCode::InvalidArgument);
    CHECK(pins.pin("trash.empty").error().code == ErrorCode::InvalidArgument);
    CHECK(pins.pin("layout.delete").error().code == ErrorCode::InvalidArgument);
    CHECK(pins.pin("keymap.reset_defaults").error().code == ErrorCode::InvalidArgument);

    CHECK(pins.move("edit.copy_path#1", 0));
    REQUIRE(pins.ids().size() == 2U);
    CHECK(pins.ids()[0] == "edit.copy_path#1");
    CHECK(pins.ids()[1] == "edit.copy_path");

    const std::string j1 = to_json(pins);
    CHECK(j1 == to_json(pins));
    const Result<Pins> parsed = pins_from_json(j1);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == pins);

    // Lenient load drops non-pinnable and duplicate entries.
    const auto messy = pins_from_json(
        R"({"version":1,"pins":["file.delete_permanently","submenu.x","edit.copy","edit.copy",42]})");
    REQUIRE(messy.has_value());
    REQUIRE(messy->ids().size() == 1U);
    CHECK(messy->ids()[0] == "edit.copy");

    // Atomic file I/O and .bad move-aside.
    test::TempDir tmp;
    const Path file = pins_file(tmp.path() / "cfg");
    CHECK(load_pins(file).error().code == ErrorCode::NotFound);
    REQUIRE(save_pins(file, pins).has_value());
    const auto loaded = load_pins(file);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == pins);

    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "not json";
    }
    CHECK(load_pins(file).error().code == ErrorCode::InvalidArgument);
    CHECK(std::filesystem::exists(Path(file.native() + ".bad")));
}

TEST_CASE("menu::build, arrange (M1/M2) and filter (M4/M5)", "[ui][menu]") {
    const CommandRegistry reg = builtins();
    CommandContext ctx;
    ctx.flags =
        ContextFlags::HasSelection | ContextFlags::HasCursorRow | ContextFlags::CursorOnFolder;

    std::vector<Item> items = build(MenuKind::ListRow, reg, ctx);
    REQUIRE_FALSE(items.empty());

    // Populate labels and an Open With child.
    for (Item& item : items) {
        if (item.kind == Item::Kind::Submenu && item.id == "submenu.open_with") {
            item.label = "Open With";
            Item child;
            child.kind = Item::Kind::OpenWith;
            child.id = "open_with:gedit.desktop";
            child.label = "Gedit";
            child.handler_id = "gedit.desktop";
            child.pinnable = false;
            item.children.push_back(std::move(child));
        } else if (item.id == "file.trash") {
            item.label = "Move to Trash";
        } else if (item.id == "file.delete_permanently") {
            item.label = "Delete Permanently";
        } else if (item.id == "edit.copy_path") {
            item.label = "Copy as Path";
        } else if (item.id == "edit.copy") {
            item.label = "Copy";
        } else if (item.id == "file.rename") {
            item.label = "Rename";
        } else if (!item.label_key.empty()) {
            item.label = item.label_key;
        }
    }

    Pins pins;
    REQUIRE(pins.pin("edit.copy_path").has_value());
    const std::vector<Item> arranged = arrange(items, pins);

    // M1: pinned item first, moved not duplicated; separator after; destructive last.
    REQUIRE_FALSE(arranged.empty());
    CHECK(arranged.front().id == "edit.copy_path");
    CHECK(arranged.front().pinned);
    CHECK(arranged.back().id == "file.delete_permanently");
    CHECK(arranged.back().danger == Danger::Destructive);
    CHECK_FALSE(arranged.back().pinnable);

    // No adjacent separators.
    for (std::size_t i = 1; i < arranged.size(); ++i) {
        CHECK_FALSE((arranged[i - 1].kind == Item::Kind::Separator &&
                     arranged[i].kind == Item::Kind::Separator));
    }

    // M4: empty query returns active = false.
    CHECK_FALSE(filter(arranged, "   ").active);

    // Typing "Trash" finds "Move to Trash" as default_row 0.
    const FilterResult trash_res = filter(arranged, "Trash");
    CHECK(trash_res.active);
    REQUIRE_FALSE(trash_res.rows.empty());
    CHECK(trash_res.rows[0].item.id == "file.trash");
    CHECK(trash_res.default_row == std::optional<std::size_t>{0});

    // Flattening submenu: typing "gedit" finds the Open With child with parent breadcrumb.
    const FilterResult gedit_res = filter(arranged, "gedit");
    REQUIRE(gedit_res.rows.size() == 1U);
    CHECK(gedit_res.rows[0].item.kind == Item::Kind::OpenWith);
    CHECK(gedit_res.rows[0].parent_label == "Open With");
    CHECK(gedit_res.default_row == std::optional<std::size_t>{0});

    // M5: when only destructive matches exist, default_row is nullopt.
    const FilterResult del_res = filter(arranged, "Delete Permanently");
    REQUIRE(del_res.rows.size() == 1U);
    CHECK(del_res.rows[0].item.danger == Danger::Destructive);
    CHECK_FALSE(del_res.default_row.has_value());
}

TEST_CASE("ui::shortcut_text formats single chords and sequences", "[ui][input]") {
    Keymap km;
    REQUIRE(load_default_keymap(km).has_value());
    CHECK(ui::primary_shortcut_text(km, "palette.open") == "Ctrl+Shift+P");
    CHECK(ui::primary_shortcut_text(km, "keymap.edit") == "Ctrl+K Ctrl+S");
    CHECK(ui::primary_shortcut_text(km, "open_with.show").empty());
}

} // namespace
} // namespace navigator::menu
