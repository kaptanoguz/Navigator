// src/ui/input/shortcut_text.hpp
//
// Shortcut display (Phase 8): every key hint in the program (menus, the palette, tooltips,
// status hints, empty states, sheet footers, the keymap editor) is built from the LIVE Keymap
// through these functions, so a rebind shows up everywhere on the next frame and nothing keeps a
// hardcoded key label of a bindable command.
//   - chord_labels: keycap legends of one chord, translated modifier names then the key legend.
//   - sequence_legends / shortcut_text: a whole sequence ("Ctrl+K Ctrl+S" is two chords).
//   - primary_shortcut: the shortcut the ui shows for a command (Keymap::sequences_for, a single
//     chord wins over a longer sequence).
// Pure, no SDL. Thread-safety: as tr() (any thread, never during set_language()).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>

namespace navigator::ui {

// Keycap legends of a chord for hints: translated modifiers (tr("key.ctrl"), ...) in the order
// Super, Ctrl, Alt, Shift, then the key legend ("A", "F5", arrows as glyphs, punctuation as the
// character, keypad keys as "Num 5").
[[nodiscard]] std::vector<std::string> chord_labels(const KeyChord& chord);

// Legend of a key alone ("Esc" is spelled "Escape", arrows are glyphs, "Num 5", ",").
[[nodiscard]] std::string key_legend(Key key);

// One entry per chord of the sequence, each the chord_labels() of it.
[[nodiscard]] std::vector<std::vector<std::string>> sequence_legends(const KeySequence& sequence);

// "Ctrl+Shift+P", "Ctrl+K Ctrl+S" with translated modifier names (plain text for tooltips and
// tests); empty for an empty sequence.
[[nodiscard]] std::string shortcut_text(const KeySequence& sequence);
[[nodiscard]] std::string shortcut_text(const KeyChord& chord);

// The shortcut shown for `command`: the first of Keymap::sequences_for. any_arg: any arg of the
// command (a hint for "Copy as path" must not vanish because a variant has its own binding);
// otherwise exactly `arg`. nullopt when unbound.
[[nodiscard]] std::optional<KeySequence>
primary_shortcut(const Keymap& keymap, std::string_view command, std::uint64_t arg, bool any_arg);
[[nodiscard]] std::string primary_shortcut_text(const Keymap& keymap, std::string_view command,
                                                std::uint64_t arg = 0, bool any_arg = true);

} // namespace navigator::ui
