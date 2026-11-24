#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>

using navigator::CommandId;
using navigator::CommandInfo;
using navigator::CommandRegistry;
using navigator::CursorArg;
using navigator::ErrorCode;

namespace {

// Ids outside the built-in range, so these tests never depend on the built-in table.
constexpr auto kTestA = static_cast<CommandId>(900);
constexpr auto kTestB = static_cast<CommandId>(901);

// Phase 0: 3 ids; Phase 2: ToggleHidden, 5 navigation, 3 list, 6 cursor; Phase 3:
// OpenInNewTab, 10 tab, 5 pane, 3 layout, 1 window; Phase 4: 2 view, GoTo, 1 sidebar,
// 4 bookmarks, 2 recents, 1 edit; Phase 5: 2 list filter, 6 search; Phase 6: view.inspect,
// 7 inspector; Phase 7: 5 edit, 7 file, 3 ops, 3 trash.
constexpr std::size_t kBuiltinCount = 18 + 20 + 11 + 8 + 8 + 18 + 8 + 19 + 8 + 5;

struct Expected {
    CommandId id;
    std::uint32_t value;
    std::string_view name;
};

// Phase 3 ids with their frozen numeric values and names (include/navigator/commands.hpp).
constexpr Expected kPhase3[] = {
    {CommandId::OpenInNewTab, 1116, "list.open_in_new_tab"},
    {CommandId::NewTab, 1200, "tab.new"},
    {CommandId::CloseTab, 1201, "tab.close"},
    {CommandId::DuplicateTab, 1202, "tab.duplicate"},
    {CommandId::CloseLeftTabs, 1203, "tab.close_left"},
    {CommandId::ReopenClosedTab, 1204, "tab.reopen_closed"},
    {CommandId::NextTab, 1205, "tab.next"},
    {CommandId::PreviousTab, 1206, "tab.previous"},
    {CommandId::SearchTabs, 1207, "tab.search"},
    {CommandId::MoveTabLeft, 1208, "tab.move_left"},
    {CommandId::MoveTabRight, 1209, "tab.move_right"},
    {CommandId::SplitRight, 1220, "pane.split_right"},
    {CommandId::SplitDown, 1221, "pane.split_down"},
    {CommandId::FocusNextPane, 1222, "pane.focus_next"},
    {CommandId::FocusPreviousPane, 1223, "pane.focus_previous"},
    {CommandId::ClosePane, 1224, "pane.close"},
    {CommandId::SaveLayout, 1240, "layout.save"},
    {CommandId::LoadLayout, 1241, "layout.load"},
    {CommandId::DeleteLayout, 1242, "layout.delete"},
    {CommandId::NewWindow, 1250, "window.new"},
};

// Phase 4 ids with their frozen numeric values and names (include/navigator/commands.hpp).
constexpr Expected kPhase4[] = {
    {CommandId::ToggleSidebar, 5, "view.toggle_sidebar"},
    {CommandId::ToggleViewLock, 6, "view.toggle_view_lock"},
    {CommandId::GoTo, 1005, "nav.goto"},
    {CommandId::FocusSidebar, 1260, "sidebar.focus"},
    {CommandId::ToggleBookmark, 1261, "bookmarks.toggle"},
    {CommandId::RenameBookmark, 1262, "bookmarks.rename"},
    {CommandId::MoveBookmarkUp, 1263, "bookmarks.move_up"},
    {CommandId::MoveBookmarkDown, 1264, "bookmarks.move_down"},
    {CommandId::ClearRecents, 1270, "recents.clear"},
    {CommandId::ToggleRecentsTracking, 1271, "recents.toggle_tracking"},
    {CommandId::CopyPath, 1280, "edit.copy_path"},
};

// Phase 5 ids with their frozen numeric values and names (include/navigator/commands.hpp).
constexpr Expected kPhase5[] = {
    {CommandId::FocusFilter, 1300, "list.filter"},
    {CommandId::ClearFilter, 1301, "list.clear_filter"},
    {CommandId::ToggleFlatten, 1302, "search.flatten"},
    {CommandId::FlattenVolume, 1303, "search.flatten_volume"},
    {CommandId::ToggleTypesPopup, 1304, "search.types"},
    {CommandId::ToggleFuzzy, 1305, "search.toggle_fuzzy"},
    {CommandId::CancelSearch, 1306, "search.cancel"},
    {CommandId::RebuildIndex, 1307, "search.rebuild_index"},
};

// Phase 6 ids with their frozen numeric values and names (include/navigator/commands.hpp).
constexpr Expected kPhase6[] = {
    {CommandId::Inspect, 1320, "view.inspect"},
    {CommandId::ToggleInspectorLock, 1321, "inspector.toggle_lock"},
    {CommandId::InspectorZoomFit, 1322, "inspector.zoom_fit"},
    {CommandId::InspectorZoom1to1, 1323, "inspector.zoom_1to1"},
    {CommandId::InspectorPlay, 1324, "inspector.play"},
    {CommandId::CycleInspectorPlacement, 1325, "inspector.cycle_placement"},
    {CommandId::ToggleInspectorGrid, 1326, "inspector.toggle_grid"},
    {CommandId::ToggleInspectorMinimap, 1327, "inspector.toggle_minimap"},
};

// Phase 7 ids with their frozen numeric values and names (include/navigator/commands.hpp).
constexpr Expected kPhase7[] = {
    {CommandId::Copy, 1281, "edit.copy"},
    {CommandId::Cut, 1282, "edit.cut"},
    {CommandId::Paste, 1283, "edit.paste"},
    {CommandId::Undo, 1284, "edit.undo"},
    {CommandId::Redo, 1285, "edit.redo"},
    {CommandId::MoveToTrash, 1340, "file.trash"},
    {CommandId::DeletePermanently, 1341, "file.delete_permanently"},
    {CommandId::Rename, 1342, "file.rename"},
    {CommandId::NewFolder, 1343, "file.new_folder"},
    {CommandId::CopyToOtherPane, 1344, "file.copy_to_other_pane"},
    {CommandId::MoveToOtherPane, 1345, "file.move_to_other_pane"},
    {CommandId::OpenTerminalHere, 1346, "file.open_terminal_here"},
    {CommandId::CancelOperation, 1347, "ops.cancel"},
    {CommandId::TogglePauseOperation, 1348, "ops.toggle_pause"},
    {CommandId::ToggleOperationsPanel, 1349, "ops.toggle_panel"},
    {CommandId::OpenTrash, 1360, "trash.open"},
    {CommandId::RestoreFromTrash, 1361, "trash.restore"},
    {CommandId::EmptyTrash, 1362, "trash.empty"},
};

} // namespace

TEST_CASE("add rejects CommandId::None", "[registry]") {
    CommandRegistry registry;
    const auto r = registry.add(CommandInfo{CommandId::None, "test.none", "cmd.test.none"});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(registry.all().empty());
}

TEST_CASE("add rejects an empty name", "[registry]") {
    CommandRegistry registry;
    const auto r = registry.add(CommandInfo{kTestA, "", "cmd."});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(registry.all().empty());
}

TEST_CASE("add rejects a duplicate id", "[registry]") {
    CommandRegistry registry;
    REQUIRE(registry.add(CommandInfo{kTestA, "test.a", "cmd.test.a"}));
    const auto r = registry.add(CommandInfo{kTestA, "test.other", "cmd.test.other"});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::AlreadyExists);
    CHECK(registry.all().size() == 1);
    CHECK(registry.find("test.other") == nullptr);
}

TEST_CASE("add rejects a duplicate name", "[registry]") {
    CommandRegistry registry;
    REQUIRE(registry.add(CommandInfo{kTestA, "test.a", "cmd.test.a"}));
    const auto r = registry.add(CommandInfo{kTestB, "test.a", "cmd.test.a"});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::AlreadyExists);
    CHECK(registry.all().size() == 1);
    CHECK(registry.find(kTestB) == nullptr);
}

TEST_CASE("lookup by id and by name", "[registry]") {
    CommandRegistry registry;
    REQUIRE(registry.add(CommandInfo{kTestA, "test.a", "cmd.test.a"}));
    REQUIRE(registry.add(CommandInfo{kTestB, "test.b", "cmd.test.b"}));

    const CommandInfo* by_id = registry.find(kTestB);
    REQUIRE(by_id != nullptr);
    CHECK(by_id->name == "test.b");
    CHECK(by_id->title_key == "cmd.test.b");

    const CommandInfo* by_name = registry.find(std::string_view{"test.a"});
    REQUIRE(by_name != nullptr);
    CHECK(by_name->id == kTestA);

    CHECK(registry.find(CommandId::Quit) == nullptr);
    CHECK(registry.find(std::string_view{"test.missing"}) == nullptr);
    CHECK(registry.find(std::string_view{"test"}) == nullptr); // no prefix matching
    CHECK(registry.find(CommandId::None) == nullptr);
}

TEST_CASE("all() preserves registration order", "[registry]") {
    CommandRegistry registry;
    REQUIRE(registry.add(CommandInfo{kTestB, "test.b", "cmd.test.b"}));
    REQUIRE(registry.add(CommandInfo{kTestA, "test.a", "cmd.test.a"}));
    const auto all = registry.all();
    REQUIRE(all.size() == 2);
    CHECK(all[0].id == kTestB);
    CHECK(all[1].id == kTestA);
}

TEST_CASE("built-in commands are registered once", "[registry][builtin]") {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    REQUIRE(registry.all().size() == kBuiltinCount);

    const CommandInfo* quit = registry.find(CommandId::Quit);
    REQUIRE(quit != nullptr);
    CHECK(quit->name == "app.quit");
    CHECK(quit->title_key == "cmd.app.quit");

    const CommandInfo* fps = registry.find(std::string_view{"view.toggle_fps_overlay"});
    REQUIRE(fps != nullptr);
    CHECK(fps->id == CommandId::ToggleFpsOverlay);
    CHECK(fps->title_key == "cmd.view.toggle_fps_overlay");

    const CommandInfo* theme = registry.find(CommandId::ToggleTheme);
    REQUIRE(theme != nullptr);
    CHECK(theme->name == "view.toggle_theme");
    CHECK(theme->title_key == "cmd.view.toggle_theme");

    CHECK(registry.find(CommandId::None) == nullptr);

    const auto again = navigator::register_builtin_commands(registry);
    REQUIRE_FALSE(again);
    CHECK(again.error().code == ErrorCode::AlreadyExists);
    CHECK(registry.all().size() == kBuiltinCount); // no partial or duplicate registration
}

TEST_CASE("built-in registration fails without side effects on a name clash",
          "[registry][builtin]") {
    CommandRegistry registry;
    REQUIRE(registry.add(CommandInfo{kTestA, "view.toggle_theme", "cmd.view.toggle_theme"}));
    const auto r = navigator::register_builtin_commands(registry);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::AlreadyExists);
    CHECK(registry.all().size() == 1);
    CHECK(registry.find(CommandId::Quit) == nullptr);
}

TEST_CASE("every built-in name matches to_string and title_key is cmd.<name>",
          "[registry][builtin]") {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    for (const CommandInfo& info : registry.all()) {
        INFO("command " << info.name);
        CHECK(info.id != CommandId::None);
        CHECK(info.title_key.starts_with("cmd."));
        CHECK(info.title_key.substr(4) == info.name);
        CHECK(navigator::to_string(info.id) == info.name);
    }
}

TEST_CASE("to_string(CommandId) values", "[to_string]") {
    CHECK(navigator::to_string(CommandId::None) == "none");
    CHECK(navigator::to_string(CommandId::Quit) == "app.quit");
    CHECK(navigator::to_string(CommandId::ToggleFpsOverlay) == "view.toggle_fps_overlay");
    CHECK(navigator::to_string(CommandId::ToggleTheme) == "view.toggle_theme");
    CHECK(navigator::to_string(CommandId::ToggleHidden) == "view.toggle_hidden");
    CHECK(navigator::to_string(CommandId::NavigateBack) == "nav.back");
    CHECK(navigator::to_string(CommandId::NavigateForward) == "nav.forward");
    CHECK(navigator::to_string(CommandId::NavigateUp) == "nav.up");
    CHECK(navigator::to_string(CommandId::Refresh) == "nav.refresh");
    CHECK(navigator::to_string(CommandId::FocusAddressBar) == "nav.focus_address_bar");
    CHECK(navigator::to_string(CommandId::Activate) == "list.activate");
    CHECK(navigator::to_string(CommandId::SelectAll) == "list.select_all");
    CHECK(navigator::to_string(CommandId::ClearSelection) == "list.clear_selection");
    CHECK(navigator::to_string(CommandId::CursorUp) == "list.cursor_up");
    CHECK(navigator::to_string(CommandId::CursorDown) == "list.cursor_down");
    CHECK(navigator::to_string(CommandId::CursorPageUp) == "list.cursor_page_up");
    CHECK(navigator::to_string(CommandId::CursorPageDown) == "list.cursor_page_down");
    CHECK(navigator::to_string(CommandId::CursorHome) == "list.cursor_home");
    CHECK(navigator::to_string(CommandId::CursorEnd) == "list.cursor_end");
    CHECK(navigator::to_string(CommandId::ToggleCursorSelected) == "list.toggle_cursor_selected");
    CHECK(navigator::to_string(CommandId::OpenPalette) == "palette.open");
    CHECK(navigator::to_string(CommandId::OpenSettings) == "settings.open");
    CHECK(navigator::to_string(CommandId::EditKeymap) == "keymap.edit");
    CHECK(navigator::to_string(CommandId::ResetKeymap) == "keymap.reset_defaults");
    CHECK(navigator::to_string(CommandId::ExportKeymap) == "keymap.export");
    CHECK(navigator::to_string(CommandId::ShowContextMenu) == "menu.show_context");
    CHECK(navigator::to_string(CommandId::OpenWith) == "open_with.show");
    CHECK(navigator::to_string(static_cast<CommandId>(7)) == "unknown");
    CHECK(navigator::to_string(static_cast<CommandId>(999)) == "unknown");
    CHECK(navigator::to_string(static_cast<CommandId>(1006)) == "unknown");
    CHECK(navigator::to_string(static_cast<CommandId>(1104)) == "unknown");
    for (const std::uint32_t gap :
         {1117U, 1199U, 1210U, 1219U, 1225U, 1239U, 1243U, 1249U, 1251U,
          1259U, 1265U, 1269U, 1272U, 1279U, 1286U, 1299U, 1308U, 1319U,
          1328U, 1339U, 1350U, 1359U, 1363U, 1369U, 1376U, 1389U, 1391U}) {
        INFO("id " << gap);
        CHECK(navigator::to_string(static_cast<CommandId>(gap)) == "unknown");
    }
    CHECK(navigator::to_string(static_cast<CommandId>(UINT32_MAX)) == "unknown");
}

TEST_CASE("CommandId numeric values are stable", "[to_string]") {
    CHECK(static_cast<std::uint32_t>(CommandId::None) == 0);
    CHECK(static_cast<std::uint32_t>(CommandId::Quit) == 1);
    CHECK(static_cast<std::uint32_t>(CommandId::ToggleFpsOverlay) == 2);
    CHECK(static_cast<std::uint32_t>(CommandId::ToggleTheme) == 3);
    CHECK(static_cast<std::uint32_t>(CommandId::ToggleHidden) == 4);
    CHECK(static_cast<std::uint32_t>(CommandId::NavigateBack) == 1000);
    CHECK(static_cast<std::uint32_t>(CommandId::NavigateForward) == 1001);
    CHECK(static_cast<std::uint32_t>(CommandId::NavigateUp) == 1002);
    CHECK(static_cast<std::uint32_t>(CommandId::Refresh) == 1003);
    CHECK(static_cast<std::uint32_t>(CommandId::FocusAddressBar) == 1004);
    CHECK(static_cast<std::uint32_t>(CommandId::Activate) == 1100);
    CHECK(static_cast<std::uint32_t>(CommandId::SelectAll) == 1101);
    CHECK(static_cast<std::uint32_t>(CommandId::ClearSelection) == 1102);
    CHECK(static_cast<std::uint32_t>(CommandId::CursorUp) == 1110);
    CHECK(static_cast<std::uint32_t>(CommandId::CursorDown) == 1111);
    CHECK(static_cast<std::uint32_t>(CommandId::CursorPageUp) == 1112);
    CHECK(static_cast<std::uint32_t>(CommandId::CursorPageDown) == 1113);
    CHECK(static_cast<std::uint32_t>(CommandId::CursorHome) == 1114);
    CHECK(static_cast<std::uint32_t>(CommandId::CursorEnd) == 1115);
    CHECK(CursorArg::None == 0);
    CHECK(CursorArg::Extend == 1);
    CHECK(navigator::NewTabArg::None == 0);
    CHECK(navigator::NewTabArg::Home == 1);
    CHECK(navigator::CopyPathArg::None == 0);
    CHECK(navigator::CopyPathArg::CurrentFolder == 1);
    CHECK(navigator::PasteArg::None == 0);
    CHECK(navigator::PasteArg::IntoCursorFolder == 1);
}

TEST_CASE("Phase 3 ids: stable values, names, registered with cmd.<name>",
          "[registry][builtin][phase3]") {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    std::uint32_t previous = 0;
    for (const Expected& e : kPhase3) {
        INFO("command " << e.name);
        CHECK(static_cast<std::uint32_t>(e.id) == e.value);
        CHECK(e.value > previous); // the table above is ascending, as the header ranges are
        previous = e.value;
        CHECK(navigator::to_string(e.id) == e.name);
        const CommandInfo* by_id = registry.find(e.id);
        REQUIRE(by_id != nullptr);
        CHECK(by_id->name == e.name);
        CHECK(by_id->title_key == std::string("cmd.") + std::string(e.name));
        const CommandInfo* by_name = registry.find(e.name);
        REQUIRE(by_name != nullptr);
        CHECK(by_name->id == e.id);
    }
    // Header ranges: tabs 1200-1219, panes 1220-1239, layouts 1240-1249, windows 1250-1259.
    for (const Expected& e : kPhase3) {
        INFO("command " << e.name);
        if (e.name.starts_with("tab.")) {
            CHECK((e.value >= 1200 && e.value <= 1219));
        } else if (e.name.starts_with("pane.")) {
            CHECK((e.value >= 1220 && e.value <= 1239));
        } else if (e.name.starts_with("layout.")) {
            CHECK((e.value >= 1240 && e.value <= 1249));
        } else if (e.name.starts_with("window.")) {
            CHECK((e.value >= 1250 && e.value <= 1259));
        } else {
            CHECK((e.value >= 1100 && e.value <= 1199)); // list.open_in_new_tab
        }
    }
}

TEST_CASE("Phase 4 ids: stable values, names, registered with cmd.<name>",
          "[registry][builtin][phase4]") {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    std::uint32_t previous = 0;
    for (const Expected& e : kPhase4) {
        INFO("command " << e.name);
        CHECK(static_cast<std::uint32_t>(e.id) == e.value);
        CHECK(e.value > previous);
        previous = e.value;
        CHECK(navigator::to_string(e.id) == e.name);
        const CommandInfo* by_id = registry.find(e.id);
        REQUIRE(by_id != nullptr);
        CHECK(by_id->name == e.name);
        CHECK(by_id->title_key == std::string("cmd.") + std::string(e.name));
        const CommandInfo* by_name = registry.find(e.name);
        REQUIRE(by_name != nullptr);
        CHECK(by_name->id == e.id);
    }
    // Header ranges: app/view 1-999, navigation 1000-1099, sidebar/bookmarks/recents
    // 1260-1279, edit/clipboard 1280-1299.
    for (const Expected& e : kPhase4) {
        INFO("command " << e.name);
        if (e.name.starts_with("view.")) {
            CHECK((e.value >= 1 && e.value <= 999));
        } else if (e.name.starts_with("nav.")) {
            CHECK((e.value >= 1000 && e.value <= 1099));
        } else if (e.name.starts_with("sidebar.") || e.name.starts_with("bookmarks.") ||
                   e.name.starts_with("recents.")) {
            CHECK((e.value >= 1260 && e.value <= 1279));
        } else {
            CHECK(e.name.starts_with("edit."));
            CHECK((e.value >= 1280 && e.value <= 1299));
        }
    }
}

TEST_CASE("Phase 5 ids: stable values, names, registered with cmd.<name>",
          "[registry][builtin][phase5]") {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    std::uint32_t previous = 0;
    for (const Expected& e : kPhase5) {
        INFO("command " << e.name);
        CHECK(static_cast<std::uint32_t>(e.id) == e.value);
        CHECK(e.value > previous);
        previous = e.value;
        CHECK(navigator::to_string(e.id) == e.name);
        const CommandInfo* by_id = registry.find(e.id);
        REQUIRE(by_id != nullptr);
        CHECK(by_id->name == e.name);
        CHECK(by_id->title_key == std::string("cmd.") + std::string(e.name));
        const CommandInfo* by_name = registry.find(e.name);
        REQUIRE(by_name != nullptr);
        CHECK(by_name->id == e.id);
        // Header range: filter, flatten, name search, index 1300-1319.
        CHECK((e.value >= 1300 && e.value <= 1319));
        CHECK((e.name.starts_with("list.") || e.name.starts_with("search.")));
    }
}

TEST_CASE("Phase 6 ids: stable values, names, registered with cmd.<name>",
          "[registry][builtin][phase6]") {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    std::uint32_t previous = 0;
    for (const Expected& e : kPhase6) {
        INFO("command " << e.name);
        CHECK(static_cast<std::uint32_t>(e.id) == e.value);
        CHECK(e.value > previous);
        previous = e.value;
        CHECK(navigator::to_string(e.id) == e.name);
        const CommandInfo* by_id = registry.find(e.id);
        REQUIRE(by_id != nullptr);
        CHECK(by_id->name == e.name);
        CHECK(by_id->title_key == std::string("cmd.") + std::string(e.name));
        const CommandInfo* by_name = registry.find(e.name);
        REQUIRE(by_name != nullptr);
        CHECK(by_name->id == e.id);
        // Header range: Inspector and previews 1320-1339.
        CHECK((e.value >= 1320 && e.value <= 1339));
        CHECK((e.name == "view.inspect" || e.name.starts_with("inspector.")));
    }
    // 1328-1339 are still free: nothing registered there yet.
    for (std::uint32_t v = 1328; v <= 1339; ++v) {
        INFO("id " << v);
        CHECK(registry.find(static_cast<CommandId>(v)) == nullptr);
    }
}

TEST_CASE("Phase 7 ids: stable values, names, registered with cmd.<name>",
          "[registry][builtin][phase7]") {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    std::uint32_t previous = 0;
    for (const Expected& e : kPhase7) {
        INFO("command " << e.name);
        CHECK(static_cast<std::uint32_t>(e.id) == e.value);
        CHECK(e.value > previous);
        previous = e.value;
        CHECK(navigator::to_string(e.id) == e.name);
        const CommandInfo* by_id = registry.find(e.id);
        REQUIRE(by_id != nullptr);
        CHECK(by_id->name == e.name);
        CHECK(by_id->title_key == std::string("cmd.") + std::string(e.name));
        const CommandInfo* by_name = registry.find(e.name);
        REQUIRE(by_name != nullptr);
        CHECK(by_name->id == e.id);
        // Header ranges: edit/clipboard 1280-1299, file operations and the operations panel
        // 1340-1359, trash 1360-1369.
        if (e.name.starts_with("edit.")) {
            CHECK((e.value >= 1281 && e.value <= 1299));
        } else if (e.name.starts_with("file.") || e.name.starts_with("ops.")) {
            CHECK((e.value >= 1340 && e.value <= 1359));
        } else {
            CHECK(e.name.starts_with("trash."));
            CHECK((e.value >= 1360 && e.value <= 1369));
        }
    }
    // The rest of each Phase 7 range is still free: nothing registered there yet.
    const auto free_range = [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t v = first; v <= last; ++v) {
            INFO("id " << v);
            CHECK(registry.find(static_cast<CommandId>(v)) == nullptr);
        }
    };
    free_range(1286, 1299);
    free_range(1350, 1359);
    free_range(1363, 1369);
    // CopyPath (Phase 4) keeps 1280; the Phase 7 edit ids follow it.
    CHECK(registry.find(CommandId::CopyPath) != nullptr);
    CHECK(static_cast<std::uint32_t>(CommandId::CopyPath) + 1 ==
          static_cast<std::uint32_t>(CommandId::Copy));
}

TEST_CASE("built-ins register in ascending CommandId order", "[registry][builtin]") {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    const auto all = registry.all();
    std::vector<std::uint32_t> ids;
    ids.reserve(all.size());
    for (const CommandInfo& info : all) {
        ids.push_back(static_cast<std::uint32_t>(info.id));
    }
    CHECK(std::ranges::is_sorted(ids));
    CHECK(std::ranges::adjacent_find(ids) == ids.end());
}
