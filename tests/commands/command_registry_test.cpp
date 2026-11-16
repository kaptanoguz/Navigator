#include <algorithm>
#include <cstddef>
#include <cstdint>
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

// Phase 0: 3 ids; Phase 2: ToggleHidden, 5 navigation, 3 list, 6 cursor.
constexpr std::size_t kBuiltinCount = 18;

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
    CHECK(navigator::to_string(static_cast<CommandId>(5)) == "unknown");
    CHECK(navigator::to_string(static_cast<CommandId>(1005)) == "unknown");
    CHECK(navigator::to_string(static_cast<CommandId>(1103)) == "unknown");
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
