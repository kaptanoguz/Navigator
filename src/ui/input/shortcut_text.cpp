// src/ui/input/shortcut_text.cpp — see shortcut_text.hpp.
#include "input/shortcut_text.hpp"

#include <navigator/i18n.hpp>

namespace navigator::ui {

std::string key_legend(Key key) {
    switch (key) {
    case Key::Up: return "\xE2\x86\x91";    // up arrow
    case Key::Down: return "\xE2\x86\x93";  // down arrow
    case Key::Left: return "\xE2\x86\x90";  // left arrow
    case Key::Right: return "\xE2\x86\x92"; // right arrow
    case Key::Minus: return "-";
    case Key::Equals: return "=";
    case Key::Comma: return ",";
    case Key::Period: return ".";
    case Key::Slash: return "/";
    case Key::Backslash: return "\\";
    case Key::Semicolon: return ";";
    case Key::Apostrophe: return "'";
    case Key::LeftBracket: return "[";
    case Key::RightBracket: return "]";
    case Key::Grave: return "`";
    case Key::Keypad0:
    case Key::Keypad1:
    case Key::Keypad2:
    case Key::Keypad3:
    case Key::Keypad4:
    case Key::Keypad5:
    case Key::Keypad6:
    case Key::Keypad7:
    case Key::Keypad8:
    case Key::Keypad9:
        return "Num " + std::string(1, static_cast<char>('0' + (static_cast<int>(key) -
                                                                static_cast<int>(Key::Keypad0))));
    case Key::KeypadPlus: return "Num +";
    case Key::KeypadMinus: return "Num -";
    case Key::KeypadMultiply: return "Num *";
    case Key::KeypadDivide: return "Num /";
    case Key::KeypadPeriod: return "Num .";
    case Key::KeypadEnter: return "Num Enter";
    case Key::MouseBack: return "Mouse 4";
    case Key::MouseForward: return "Mouse 5";
    default: return std::string(to_string(key));
    }
}

std::vector<std::string> chord_labels(const KeyChord& chord) {
    std::vector<std::string> out;
    if (has(chord.mods, Modifiers::Super)) {
        out.emplace_back(tr("key.super"));
    }
    if (has(chord.mods, Modifiers::Ctrl)) {
        out.emplace_back(tr("key.ctrl"));
    }
    if (has(chord.mods, Modifiers::Alt)) {
        out.emplace_back(tr("key.alt"));
    }
    if (has(chord.mods, Modifiers::Shift)) {
        out.emplace_back(tr("key.shift"));
    }
    out.push_back(key_legend(chord.key));
    return out;
}

std::vector<std::vector<std::string>> sequence_legends(const KeySequence& sequence) {
    std::vector<std::vector<std::string>> out;
    out.reserve(sequence.chords.size());
    for (const KeyChord& c : sequence.chords) {
        out.push_back(chord_labels(c));
    }
    return out;
}

std::string shortcut_text(const KeyChord& chord) {
    std::string out;
    for (const std::string& part : chord_labels(chord)) {
        if (!out.empty()) {
            out += '+';
        }
        out += part;
    }
    return out;
}

std::string shortcut_text(const KeySequence& sequence) {
    std::string out;
    for (const KeyChord& c : sequence.chords) {
        if (!out.empty()) {
            out += ' ';
        }
        out += shortcut_text(c);
    }
    return out;
}

std::optional<KeySequence> primary_shortcut(const Keymap& keymap, std::string_view command,
                                            std::uint64_t arg, bool any_arg) {
    std::vector<KeySequence> all =
        any_arg ? keymap.sequences_for(command) : keymap.sequences_for(command, arg);
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::string primary_shortcut_text(const Keymap& keymap, std::string_view command, std::uint64_t arg,
                                  bool any_arg) {
    const std::optional<KeySequence> s = primary_shortcut(keymap, command, arg, any_arg);
    return s ? shortcut_text(*s) : std::string();
}

} // namespace navigator::ui
