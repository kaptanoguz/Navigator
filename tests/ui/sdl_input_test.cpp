// SDL key/mouse -> navigator Key/Modifiers translation (pure functions), and the default keymap
// reached through it.
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>
#include <navigator/i18n.hpp>

#include "input/sdl_input.hpp"

using namespace navigator;
using namespace navigator::ui;

TEST_CASE("Letters match by keycode, folded to lowercase", "[keys]") {
    CHECK(translate_key(SDLK_A, SDL_SCANCODE_A) == Key::A);
    CHECK(translate_key('A', SDL_SCANCODE_A) == Key::A);    // Shift may report the capital legend
    CHECK(translate_key(SDLK_Z, SDL_SCANCODE_Y) == Key::Z); // German layout: legend wins
    CHECK(translate_key(SDLK_Q, SDL_SCANCODE_A) == Key::Q); // AZERTY
    CHECK(translate_key(SDLK_5, SDL_SCANCODE_5) == Key::Digit5);
    CHECK(translate_key(SDLK_F5, SDL_SCANCODE_F5) == Key::F5);
    CHECK(translate_key(SDLK_F12, SDL_SCANCODE_F12) == Key::F12);
    CHECK(translate_key(SDLK_F24, SDL_SCANCODE_F24) == Key::F24);
}

TEST_CASE("Navigation and editing keys", "[keys]") {
    CHECK(translate_key(SDLK_UP, SDL_SCANCODE_UP) == Key::Up);
    CHECK(translate_key(SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN) == Key::PageDown);
    CHECK(translate_key(SDLK_HOME, SDL_SCANCODE_HOME) == Key::Home);
    CHECK(translate_key(SDLK_RETURN, SDL_SCANCODE_RETURN) == Key::Enter);
    CHECK(translate_key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE) == Key::Backspace);
    CHECK(translate_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE) == Key::Escape);
    CHECK(translate_key(SDLK_COMMA, SDL_SCANCODE_COMMA) == Key::Comma);
    CHECK(translate_key(SDLK_GRAVE, SDL_SCANCODE_GRAVE) == Key::Grave);
}

TEST_CASE("Keypad: only Enter is distinct, digits and operators are unknown", "[keys]") {
    CHECK(translate_key(SDLK_KP_ENTER, SDL_SCANCODE_KP_ENTER) == Key::KeypadEnter);
    CHECK(translate_key(SDLK_KP_1, SDL_SCANCODE_KP_1) == Key::Unknown);
    CHECK(translate_key(SDLK_1, SDL_SCANCODE_KP_1) == Key::Unknown); // NumLock legend
    CHECK(translate_key(SDLK_KP_PLUS, SDL_SCANCODE_KP_PLUS) == Key::Unknown);
}

TEST_CASE("Non-Latin legends fall back to the physical position", "[keys]") {
    // Turkish Q: dotless ı sits on the US I key, ş on ;, ö on ',', ç on '.', ğ on '[', ü on ']'.
    CHECK(translate_key(0x0131U, SDL_SCANCODE_I) == Key::I);
    CHECK(translate_key(0x015FU, SDL_SCANCODE_SEMICOLON) == Key::Semicolon);
    CHECK(translate_key(0x00F6U, SDL_SCANCODE_COMMA) == Key::Comma);
    CHECK(translate_key(0x00E7U, SDL_SCANCODE_PERIOD) == Key::Period);
    CHECK(translate_key(0x011FU, SDL_SCANCODE_LEFTBRACKET) == Key::LeftBracket);
    // Any non-ASCII legend on the US Q key still means Q for Ctrl+<that key>.
    CHECK(translate_key(0x011FU, SDL_SCANCODE_Q) == Key::Q);
    // Cyrillic й on the US Q key.
    CHECK(translate_key(0x0439U, SDL_SCANCODE_Q) == Key::Q);
    // Turkish Q: dotted i on the US apostrophe key is the ASCII legend 'i' -> I.
    CHECK(translate_key(SDLK_I, SDL_SCANCODE_APOSTROPHE) == Key::I);
}

TEST_CASE("ASCII legends without a Key value never steal a position", "[keys]") {
    CHECK(translate_key('<', SDL_SCANCODE_NONUSBACKSLASH) == Key::Unknown);
    CHECK(translate_key('*', SDL_SCANCODE_MINUS) == Key::Unknown); // not Minus
    CHECK(translate_key(SDLK_MEDIA_PLAY, SDL_SCANCODE_MEDIA_PLAY) == Key::Unknown);
}

TEST_CASE("Modifiers merge sides and ignore locks and AltGr", "[keys]") {
    CHECK(translate_modifiers(SDL_KMOD_NONE) == Modifiers::None);
    CHECK(translate_modifiers(SDL_KMOD_LCTRL) == Modifiers::Ctrl);
    CHECK(translate_modifiers(SDL_KMOD_RCTRL | SDL_KMOD_LSHIFT) ==
          (Modifiers::Ctrl | Modifiers::Shift));
    CHECK(translate_modifiers(SDL_KMOD_LALT) == Modifiers::Alt);
    CHECK(translate_modifiers(SDL_KMOD_RALT) == Modifiers::Alt);
    CHECK(translate_modifiers(SDL_KMOD_LGUI) == Modifiers::Super);
    CHECK(translate_modifiers(SDL_KMOD_CAPS | SDL_KMOD_NUM) == Modifiers::None);
    CHECK(translate_modifiers(SDL_KMOD_MODE) == Modifiers::None);
    CHECK(translate_modifiers(SDL_KMOD_MODE | SDL_KMOD_RALT) == Modifiers::None); // AltGr
    CHECK(translate_modifiers(SDL_KMOD_MODE | SDL_KMOD_RALT | SDL_KMOD_LALT) == Modifiers::Alt);
}

TEST_CASE("Mouse side buttons map to MouseBack/MouseForward", "[keys]") {
    CHECK(translate_mouse_button(SDL_BUTTON_X1) == Key::MouseBack);
    CHECK(translate_mouse_button(SDL_BUTTON_X2) == Key::MouseForward);
    CHECK(translate_mouse_button(SDL_BUTTON_LEFT) == Key::Unknown);
}

TEST_CASE("Translated chords resolve through the default keymap", "[keys]") {
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    const auto resolve = [&](SDL_Keycode k, SDL_Scancode s, SDL_Keymod m) {
        return resolve_command(keymap, registry, translate_chord(k, s, m));
    };
    CHECK(resolve(SDLK_Q, SDL_SCANCODE_Q, SDL_KMOD_LCTRL) == Command{CommandId::Quit, 0});
    CHECK(resolve(SDLK_LEFT, SDL_SCANCODE_LEFT, SDL_KMOD_LALT) ==
          Command{CommandId::NavigateBack, 0});
    CHECK(resolve(SDLK_UP, SDL_SCANCODE_UP, SDL_KMOD_RALT) == Command{CommandId::NavigateUp, 0});
    CHECK_FALSE(resolve(SDLK_UP, SDL_SCANCODE_UP, SDL_KMOD_RALT | SDL_KMOD_MODE) ==
                Command{CommandId::NavigateUp, 0}); // AltGr+Up is plain Up
    CHECK(resolve(SDLK_DOWN, SDL_SCANCODE_DOWN, SDL_KMOD_LSHIFT) ==
          Command{CommandId::CursorDown, CursorArg::Extend});
    CHECK(resolve(SDLK_KP_ENTER, SDL_SCANCODE_KP_ENTER, SDL_KMOD_NUM) ==
          Command{CommandId::Activate, 0});
    CHECK(resolve(SDLK_H, SDL_SCANCODE_H, SDL_KMOD_LCTRL) == Command{CommandId::ToggleHidden, 0});
    // Turkish Q layout: Ctrl + the key labelled "I" (keycode ı) is Ctrl+I, not unbound noise.
    const KeyChord tr_i = translate_chord(0x0131U, SDL_SCANCODE_I, SDL_KMOD_LCTRL);
    CHECK(tr_i == KeyChord{Key::I, Modifiers::Ctrl});
    const auto back = resolve_command(
        keymap, registry, KeyChord{translate_mouse_button(SDL_BUTTON_X1), Modifiers::None});
    CHECK(back == Command{CommandId::NavigateBack, 0});
}

TEST_CASE("Chord labels translate modifiers and draw arrows", "[keys]") {
    CHECK(chord_labels(KeyChord{Key::L, Modifiers::Ctrl | Modifiers::Shift}) ==
          std::vector<std::string>{"Ctrl", "Shift", "L"});
    CHECK(chord_labels(KeyChord{Key::Up, Modifiers::Alt}) ==
          std::vector<std::string>{"Alt", "\xE2\x86\x91"});
    CHECK(chord_labels(KeyChord{Key::F12, Modifiers::None}) == std::vector<std::string>{"F12"});
    CHECK(chord_labels(KeyChord{Key::A, Modifiers::Super | Modifiers::Ctrl}) ==
          std::vector<std::string>{"Super", "Ctrl", "A"});
}
