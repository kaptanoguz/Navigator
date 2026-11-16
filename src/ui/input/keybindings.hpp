// src/ui/input/keybindings.hpp
//
// Phase 0 default key bindings (docs/keymap.md): chord -> command NAME. Names are resolved to
// CommandIds through CommandRegistry::find(name); widgets never hardcode behavior behind a key.
// Pure data, no SDL: the shell translates SDL key events into `Key` + `Modifiers` first.
// Thread-safety: immutable data and pure functions.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>

namespace navigator::ui {

// Keys the Phase 0 bindings need. Extended as commands land.
enum class Key : std::uint8_t {
    Unknown,
    L,
    Q,
    F12,
};

enum class Modifier : std::uint8_t {
    None = 0,
    Ctrl = 1U << 0U,
    Shift = 1U << 1U,
    Alt = 1U << 2U,
    Super = 1U << 3U,
};

using Modifiers = std::uint8_t; // bit set of Modifier

[[nodiscard]] constexpr Modifiers mods(Modifier m) noexcept { return static_cast<Modifiers>(m); }
[[nodiscard]] constexpr Modifiers operator|(Modifier a, Modifier b) noexcept {
    return static_cast<Modifiers>(mods(a) | mods(b));
}

struct KeyBinding {
    Key key = Key::Unknown;
    Modifiers modifiers = 0;
    std::string_view command; // CommandInfo::name, e.g. "app.quit"
};

// The default table, in display order.
[[nodiscard]] Span<const KeyBinding> default_bindings() noexcept;

// First binding whose key and exact modifier set match, or nullptr.
[[nodiscard]] const KeyBinding* match_binding(Key key, Modifiers modifiers) noexcept;

// Resolves a chord to a registered command; CommandId::None when unbound or unregistered.
[[nodiscard]] CommandId resolve_binding(const CommandRegistry& registry, Key key,
                                        Modifiers modifiers) noexcept;

// First binding for a command name (used to render key hints), or nullptr.
[[nodiscard]] const KeyBinding* binding_for(std::string_view command) noexcept;

// Keycap labels for a binding, modifiers first: {"Ctrl", "Shift", "L"}. Not translated: these are
// the legends printed on Linux keyboards in both English and Turkish layouts.
[[nodiscard]] std::vector<std::string_view> chord_labels(const KeyBinding& binding);
[[nodiscard]] std::string_view key_label(Key key) noexcept;

} // namespace navigator::ui
