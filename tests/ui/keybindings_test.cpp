// Default key bindings resolve to registered built-in commands.
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>

#include "input/keybindings.hpp"

using namespace navigator;
using namespace navigator::ui;

TEST_CASE("Every default binding names a registered built-in command", "[keys]") {
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    REQUIRE_FALSE(default_bindings().empty());
    for (const KeyBinding& b : default_bindings()) {
        INFO(b.command);
        const CommandInfo* info = registry.find(b.command);
        REQUIRE(info != nullptr);
        CHECK(resolve_binding(registry, b.key, b.modifiers) == info->id);
    }
}

TEST_CASE("Phase 0 chords match docs/keymap.md", "[keys]") {
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    CHECK(resolve_binding(registry, Key::Q, mods(Modifier::Ctrl)) == CommandId::Quit);
    CHECK(resolve_binding(registry, Key::F12, 0) == CommandId::ToggleFpsOverlay);
    CHECK(resolve_binding(registry, Key::L, Modifier::Ctrl | Modifier::Shift) ==
          CommandId::ToggleTheme);
}

TEST_CASE("Modifiers must match exactly", "[keys]") {
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    CHECK(resolve_binding(registry, Key::Q, 0) == CommandId::None);
    CHECK(resolve_binding(registry, Key::Q, Modifier::Ctrl | Modifier::Shift) == CommandId::None);
    CHECK(resolve_binding(registry, Key::L, mods(Modifier::Ctrl)) == CommandId::None);
    CHECK(resolve_binding(registry, Key::Unknown, 0) == CommandId::None);
}

TEST_CASE("Unregistered commands resolve to None", "[keys]") {
    const CommandRegistry empty;
    CHECK(resolve_binding(empty, Key::Q, mods(Modifier::Ctrl)) == CommandId::None);
}

TEST_CASE("Chord labels list modifiers first", "[keys]") {
    const KeyBinding* theme = binding_for("view.toggle_theme");
    REQUIRE(theme != nullptr);
    CHECK(chord_labels(*theme) == std::vector<std::string_view>{"Ctrl", "Shift", "L"});
    const KeyBinding* fps = binding_for("view.toggle_fps_overlay");
    REQUIRE(fps != nullptr);
    CHECK(chord_labels(*fps) == std::vector<std::string_view>{"F12"});
    CHECK(binding_for("no.such.command") == nullptr);
}
