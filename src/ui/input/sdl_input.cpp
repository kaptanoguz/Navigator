// src/ui/input/sdl_input.cpp — see sdl_input.hpp.
#include "input/sdl_input.hpp"

namespace navigator::ui {
namespace {

Key letter(int index) noexcept { return static_cast<Key>(static_cast<int>(Key::A) + index); }

Key from_keycode(SDL_Keycode k) noexcept {
    if (k >= 'A' && k <= 'Z') {
        k += 'a' - 'A';
    }
    if (k >= 'a' && k <= 'z') {
        return letter(static_cast<int>(k - 'a'));
    }
    if (k >= '0' && k <= '9') {
        return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(k - '0'));
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(k - SDLK_F1));
    }
    if (k >= SDLK_F13 && k <= SDLK_F24) {
        return static_cast<Key>(static_cast<int>(Key::F13) + static_cast<int>(k - SDLK_F13));
    }
    switch (k) {
    case SDLK_UP: return Key::Up;
    case SDLK_DOWN: return Key::Down;
    case SDLK_LEFT: return Key::Left;
    case SDLK_RIGHT: return Key::Right;
    case SDLK_PAGEUP: return Key::PageUp;
    case SDLK_PAGEDOWN: return Key::PageDown;
    case SDLK_HOME: return Key::Home;
    case SDLK_END: return Key::End;
    case SDLK_RETURN: return Key::Enter;
    case SDLK_KP_ENTER: return Key::KeypadEnter;
    case SDLK_ESCAPE: return Key::Escape;
    case SDLK_BACKSPACE: return Key::Backspace;
    case SDLK_DELETE: return Key::Delete;
    case SDLK_TAB: return Key::Tab;
    case SDLK_SPACE: return Key::Space;
    case SDLK_INSERT: return Key::Insert;
    case SDLK_MINUS: return Key::Minus;
    case SDLK_EQUALS: return Key::Equals;
    case SDLK_COMMA: return Key::Comma;
    case SDLK_PERIOD: return Key::Period;
    case SDLK_SLASH: return Key::Slash;
    case SDLK_BACKSLASH: return Key::Backslash;
    case SDLK_SEMICOLON: return Key::Semicolon;
    case SDLK_APOSTROPHE: return Key::Apostrophe;
    case SDLK_LEFTBRACKET: return Key::LeftBracket;
    case SDLK_RIGHTBRACKET: return Key::RightBracket;
    case SDLK_GRAVE: return Key::Grave;
    default: return Key::Unknown;
    }
}

// US-position legend of a printable physical key.
Key from_scancode(SDL_Scancode s) noexcept {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        return letter(static_cast<int>(s - SDL_SCANCODE_A));
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<int>(Key::Digit1) +
                                static_cast<int>(s - SDL_SCANCODE_1));
    }
    switch (s) {
    case SDL_SCANCODE_0: return Key::Digit0;
    case SDL_SCANCODE_MINUS: return Key::Minus;
    case SDL_SCANCODE_EQUALS: return Key::Equals;
    case SDL_SCANCODE_COMMA: return Key::Comma;
    case SDL_SCANCODE_PERIOD: return Key::Period;
    case SDL_SCANCODE_SLASH: return Key::Slash;
    case SDL_SCANCODE_BACKSLASH: return Key::Backslash;
    case SDL_SCANCODE_SEMICOLON: return Key::Semicolon;
    case SDL_SCANCODE_APOSTROPHE: return Key::Apostrophe;
    case SDL_SCANCODE_LEFTBRACKET: return Key::LeftBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return Key::RightBracket;
    case SDL_SCANCODE_GRAVE: return Key::Grave;
    default: return Key::Unknown;
    }
}

bool is_keypad(SDL_Scancode s) noexcept {
    return (s >= SDL_SCANCODE_KP_DIVIDE && s <= SDL_SCANCODE_KP_PERIOD) ||
           s == SDL_SCANCODE_KP_EQUALS || s == SDL_SCANCODE_KP_COMMA ||
           s == SDL_SCANCODE_KP_EQUALSAS400 ||
           (s >= SDL_SCANCODE_KP_00 && s <= SDL_SCANCODE_KP_HEXADECIMAL);
}

// The physical keypad key (docs: commands.hpp "Keypad keys"): identity does not depend on
// NumLock. Keypad keys without a Key value (KP_EQUALS, KP_COMMA, KP_00, ...) are Unknown.
Key from_keypad_scancode(SDL_Scancode s) noexcept {
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) {
        return static_cast<Key>(static_cast<int>(Key::Keypad1) +
                                static_cast<int>(s - SDL_SCANCODE_KP_1));
    }
    switch (s) {
    case SDL_SCANCODE_KP_0: return Key::Keypad0;
    case SDL_SCANCODE_KP_PLUS: return Key::KeypadPlus;
    case SDL_SCANCODE_KP_MINUS: return Key::KeypadMinus;
    case SDL_SCANCODE_KP_MULTIPLY: return Key::KeypadMultiply;
    case SDL_SCANCODE_KP_DIVIDE: return Key::KeypadDivide;
    case SDL_SCANCODE_KP_PERIOD: return Key::KeypadPeriod;
    case SDL_SCANCODE_KP_ENTER: return Key::KeypadEnter;
    default: return Key::Unknown;
    }
}

} // namespace

Key translate_key(SDL_Keycode keycode, SDL_Scancode scancode) noexcept {
    if (scancode == SDL_SCANCODE_KP_ENTER || keycode == SDLK_KP_ENTER) {
        return Key::KeypadEnter;
    }
    if (is_keypad(scancode)) {
        return from_keypad_scancode(scancode); // by the physical key, NumLock-independent
    }
    if (keycode == SDLK_APPLICATION || scancode == SDL_SCANCODE_APPLICATION ||
        keycode == SDLK_MENU) {
        return Key::Menu; // the context-menu key
    }
    const Key key = from_keycode(keycode);
    if (key != Key::Unknown) {
        return key;
    }
    if (keycode < 0x80U) {
        return Key::Unknown; // an ASCII legend without a Key value never steals a position
    }
    if ((keycode & SDLK_SCANCODE_MASK) != 0U) {
        return Key::Unknown; // non-printable key with no Key value (media keys, ...)
    }
    return from_scancode(scancode); // non-ASCII legend (ı, ş, ö, Cyrillic, ...)
}

Modifiers translate_modifiers(SDL_Keymod mod) noexcept {
    Modifiers out = Modifiers::None;
    if ((mod & SDL_KMOD_CTRL) != 0U) {
        out |= Modifiers::Ctrl;
    }
    if ((mod & SDL_KMOD_SHIFT) != 0U) {
        out |= Modifiers::Shift;
    }
    // AltGr: some X11/Wayland setups report it as MODE plus RALT. AltGr is not Alt.
    const bool altgr = (mod & SDL_KMOD_MODE) != 0U;
    if ((mod & SDL_KMOD_LALT) != 0U || ((mod & SDL_KMOD_RALT) != 0U && !altgr)) {
        out |= Modifiers::Alt;
    }
    if ((mod & SDL_KMOD_GUI) != 0U) {
        out |= Modifiers::Super;
    }
    return out;
}

KeyChord translate_chord(SDL_Keycode keycode, SDL_Scancode scancode, SDL_Keymod mod) noexcept {
    return KeyChord{translate_key(keycode, scancode), translate_modifiers(mod)};
}

Key translate_mouse_button(std::uint8_t button) noexcept {
    switch (button) {
    case SDL_BUTTON_X1: return Key::MouseBack;
    case SDL_BUTTON_X2: return Key::MouseForward;
    default: return Key::Unknown;
    }
}

} // namespace navigator::ui
