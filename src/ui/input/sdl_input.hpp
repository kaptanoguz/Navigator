// src/ui/input/sdl_input.hpp
//
// SDL key/mouse events -> navigator Key / Modifiers / KeyChord, following the translation rules
// in navigator/commands.hpp ("Keymap"):
//   - match by keycode (the legend of the active layout); ASCII letters folded to lowercase;
//   - a keycode that is not ASCII (Turkish ı ş ğ ü ö ç, Cyrillic, ...) falls back to the
//     physical key position (scancode); an ASCII legend with no Key value does not fall back;
//   - keypad keys (Phase 8) map by the PHYSICAL keypad key (scancode), whatever NumLock does:
//     KP_0..KP_9 -> Keypad0..Keypad9, KP_PLUS/MINUS/MULTIPLY/DIVIDE/PERIOD -> KeypadPlus..
//     KeypadPeriod, KP_ENTER -> KeypadEnter; a keypad key never falls back to the digit row or
//     to the navigation key it produces without NumLock; the Menu (application) key -> Key::Menu;
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

#include "input/shortcut_text.hpp"

namespace navigator::ui {

[[nodiscard]] Key translate_key(SDL_Keycode keycode, SDL_Scancode scancode) noexcept;
[[nodiscard]] Modifiers translate_modifiers(SDL_Keymod mod) noexcept;
[[nodiscard]] KeyChord translate_chord(SDL_Keycode keycode, SDL_Scancode scancode,
                                       SDL_Keymod mod) noexcept;
// SDL mouse button index -> MouseBack / MouseForward, or Key::Unknown.
[[nodiscard]] Key translate_mouse_button(std::uint8_t button) noexcept;

// chord_labels() (key hint legends) lives in input/shortcut_text.hpp, included here for the
// existing callers.

} // namespace navigator::ui
