// src/ui/input/sdl_input.hpp
//
// SDL key/mouse events -> navigator Key / Modifiers / KeyChord, following the translation rules
// in navigator/commands.hpp ("Keymap"):
//   - match by keycode (the legend of the active layout); ASCII letters folded to lowercase;
//   - a keycode that is not ASCII (Turkish ı ş ğ ü ö ç, Cyrillic, ...) falls back to the
//     physical key position (scancode); an ASCII legend with no Key value does not fall back;
//   - keypad digits/operators -> Key::Unknown, KP_ENTER -> Key::KeypadEnter;
//   - left/right modifiers merged; CapsLock, NumLock and AltGr (SDL_KMOD_MODE) are never
//     modifiers, and AltGr is not Alt;
//   - mouse X1/X2 -> MouseBack/MouseForward.
// Also: display labels for key hints (translated modifier names, arrow glyphs).
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <navigator/commands.hpp>

namespace navigator::ui {

[[nodiscard]] Key translate_key(SDL_Keycode keycode, SDL_Scancode scancode) noexcept;
[[nodiscard]] Modifiers translate_modifiers(SDL_Keymod mod) noexcept;
[[nodiscard]] KeyChord translate_chord(SDL_Keycode keycode, SDL_Scancode scancode,
                                       SDL_Keymod mod) noexcept;
// SDL mouse button index -> MouseBack / MouseForward, or Key::Unknown.
[[nodiscard]] Key translate_mouse_button(std::uint8_t button) noexcept;

// Keycap legends of a chord for hints: translated modifiers (tr("key.ctrl"), ...) in the order
// Super, Ctrl, Alt, Shift, then the key legend ("A", "F5", "↑", "Enter", ...).
[[nodiscard]] std::vector<std::string> chord_labels(const KeyChord& chord);

} // namespace navigator::ui
