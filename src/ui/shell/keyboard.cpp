// src/ui/shell/keyboard.cpp — see keyboard.hpp.
#include "shell/keyboard.hpp"

namespace navigator::ui {
namespace {

Key from_keycode(SDL_Keycode k) noexcept {
    // With Shift held SDL may report the shifted (uppercase) legend; fold ASCII to lowercase.
    if (k >= 'A' && k <= 'Z') {
        k += 'a' - 'A';
    }
    switch (k) {
    case SDLK_L: return Key::L;
    case SDLK_Q: return Key::Q;
    case SDLK_F12: return Key::F12;
    default: return Key::Unknown;
    }
}

Key from_scancode(SDL_Scancode s) noexcept {
    switch (s) {
    case SDL_SCANCODE_L: return Key::L;
    case SDL_SCANCODE_Q: return Key::Q;
    case SDL_SCANCODE_F12: return Key::F12;
    default: return Key::Unknown;
    }
}

bool is_latin_letter(SDL_Keycode k) noexcept {
    return (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z');
}

} // namespace

Key translate_key(SDL_Keycode keycode, SDL_Scancode scancode) noexcept {
    const Key key = from_keycode(keycode);
    if (key != Key::Unknown || is_latin_letter(keycode)) {
        return key; // a Latin legend that is not bound: do not steal another key's position
    }
    return from_scancode(scancode);
}

Modifiers translate_modifiers(SDL_Keymod mod) noexcept {
    Modifiers out = 0;
    if ((mod & SDL_KMOD_CTRL) != 0U) {
        out |= mods(Modifier::Ctrl);
    }
    if ((mod & SDL_KMOD_SHIFT) != 0U) {
        out |= mods(Modifier::Shift);
    }
    if ((mod & SDL_KMOD_ALT) != 0U) {
        out |= mods(Modifier::Alt);
    }
    if ((mod & SDL_KMOD_GUI) != 0U) {
        out |= mods(Modifier::Super);
    }
    return out;
}

} // namespace navigator::ui
