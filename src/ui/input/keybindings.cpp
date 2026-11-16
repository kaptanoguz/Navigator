// src/ui/input/keybindings.cpp — Phase 0 default bindings. Keep in sync with docs/keymap.md.
#include "input/keybindings.hpp"

#include <array>

namespace navigator::ui {
namespace {

constexpr std::array kDefaults{
    KeyBinding{Key::Q, mods(Modifier::Ctrl), "app.quit"},
    KeyBinding{Key::F12, mods(Modifier::None), "view.toggle_fps_overlay"},
    KeyBinding{Key::L, Modifier::Ctrl | Modifier::Shift, "view.toggle_theme"},
};

} // namespace

Span<const KeyBinding> default_bindings() noexcept { return kDefaults; }

const KeyBinding* match_binding(Key key, Modifiers modifiers) noexcept {
    if (key == Key::Unknown) {
        return nullptr;
    }
    for (const KeyBinding& b : kDefaults) {
        if (b.key == key && b.modifiers == modifiers) {
            return &b;
        }
    }
    return nullptr;
}

CommandId resolve_binding(const CommandRegistry& registry, Key key, Modifiers modifiers) noexcept {
    const KeyBinding* binding = match_binding(key, modifiers);
    if (binding == nullptr) {
        return CommandId::None;
    }
    const CommandInfo* info = registry.find(binding->command);
    return info == nullptr ? CommandId::None : info->id;
}

const KeyBinding* binding_for(std::string_view command) noexcept {
    for (const KeyBinding& b : kDefaults) {
        if (b.command == command) {
            return &b;
        }
    }
    return nullptr;
}

std::string_view key_label(Key key) noexcept {
    switch (key) {
    case Key::Unknown: return "?";
    case Key::L: return "L";
    case Key::Q: return "Q";
    case Key::F12: return "F12";
    }
    return "?";
}

std::vector<std::string_view> chord_labels(const KeyBinding& binding) {
    std::vector<std::string_view> out;
    const auto has = [&binding](Modifier m) { return (binding.modifiers & mods(m)) != 0U; };
    if (has(Modifier::Super)) {
        out.emplace_back("Super");
    }
    if (has(Modifier::Ctrl)) {
        out.emplace_back("Ctrl");
    }
    if (has(Modifier::Alt)) {
        out.emplace_back("Alt");
    }
    if (has(Modifier::Shift)) {
        out.emplace_back("Shift");
    }
    out.push_back(key_label(binding.key));
    return out;
}

} // namespace navigator::ui
