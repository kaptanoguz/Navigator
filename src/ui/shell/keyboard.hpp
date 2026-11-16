// src/ui/shell/keyboard.hpp
//
// SDL key event -> (Key, Modifiers) for the binding table. Layout-aware: chords match by keycode
// (the legend the user sees), falling back to the physical key (scancode) when the layout does
// not produce a Latin letter there, so Turkish Q and F layouts both work.
// Thread-safety: pure functions.
#pragma once

#include <SDL3/SDL.h>

#include "input/keybindings.hpp"

namespace navigator::ui {

[[nodiscard]] Key translate_key(SDL_Keycode keycode, SDL_Scancode scancode) noexcept;
[[nodiscard]] Modifiers translate_modifiers(SDL_Keymod mod) noexcept;

} // namespace navigator::ui
