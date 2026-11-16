// Key names, chord text (to_string / parse_key_chord), Keymap and resolve_command.
//
// Keymap is UI-thread only; the free functions here are pure (see navigator/commands.hpp).
// Lookups are linear: a keymap holds tens of bindings, and bind order must be preserved for
// chords_for() and bindings().
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <navigator/commands.hpp>

namespace navigator {
namespace {

struct KeyName {
    Key key;
    std::string_view name;
};

// Every Key except Unknown, in enum order. to_string(Key) and parse_key_chord() both use it, so
// the two stay inverse by construction.
constexpr std::array kKeyNames{
    KeyName{Key::A, "A"},
    KeyName{Key::B, "B"},
    KeyName{Key::C, "C"},
    KeyName{Key::D, "D"},
    KeyName{Key::E, "E"},
    KeyName{Key::F, "F"},
    KeyName{Key::G, "G"},
    KeyName{Key::H, "H"},
    KeyName{Key::I, "I"},
    KeyName{Key::J, "J"},
    KeyName{Key::K, "K"},
    KeyName{Key::L, "L"},
    KeyName{Key::M, "M"},
    KeyName{Key::N, "N"},
    KeyName{Key::O, "O"},
    KeyName{Key::P, "P"},
    KeyName{Key::Q, "Q"},
    KeyName{Key::R, "R"},
    KeyName{Key::S, "S"},
    KeyName{Key::T, "T"},
    KeyName{Key::U, "U"},
    KeyName{Key::V, "V"},
    KeyName{Key::W, "W"},
    KeyName{Key::X, "X"},
    KeyName{Key::Y, "Y"},
    KeyName{Key::Z, "Z"},
    KeyName{Key::Digit0, "0"},
    KeyName{Key::Digit1, "1"},
    KeyName{Key::Digit2, "2"},
    KeyName{Key::Digit3, "3"},
    KeyName{Key::Digit4, "4"},
    KeyName{Key::Digit5, "5"},
    KeyName{Key::Digit6, "6"},
    KeyName{Key::Digit7, "7"},
    KeyName{Key::Digit8, "8"},
    KeyName{Key::Digit9, "9"},
    KeyName{Key::F1, "F1"},
    KeyName{Key::F2, "F2"},
    KeyName{Key::F3, "F3"},
    KeyName{Key::F4, "F4"},
    KeyName{Key::F5, "F5"},
    KeyName{Key::F6, "F6"},
    KeyName{Key::F7, "F7"},
    KeyName{Key::F8, "F8"},
    KeyName{Key::F9, "F9"},
    KeyName{Key::F10, "F10"},
    KeyName{Key::F11, "F11"},
    KeyName{Key::F12, "F12"},
    KeyName{Key::F13, "F13"},
    KeyName{Key::F14, "F14"},
    KeyName{Key::F15, "F15"},
    KeyName{Key::F16, "F16"},
    KeyName{Key::F17, "F17"},
    KeyName{Key::F18, "F18"},
    KeyName{Key::F19, "F19"},
    KeyName{Key::F20, "F20"},
    KeyName{Key::F21, "F21"},
    KeyName{Key::F22, "F22"},
    KeyName{Key::F23, "F23"},
    KeyName{Key::F24, "F24"},
    KeyName{Key::Up, "Up"},
    KeyName{Key::Down, "Down"},
    KeyName{Key::Left, "Left"},
    KeyName{Key::Right, "Right"},
    KeyName{Key::PageUp, "PageUp"},
    KeyName{Key::PageDown, "PageDown"},
    KeyName{Key::Home, "Home"},
    KeyName{Key::End, "End"},
    KeyName{Key::Enter, "Enter"},
    KeyName{Key::KeypadEnter, "KeypadEnter"},
    KeyName{Key::Escape, "Escape"},
    KeyName{Key::Backspace, "Backspace"},
    KeyName{Key::Delete, "Delete"},
    KeyName{Key::Tab, "Tab"},
    KeyName{Key::Space, "Space"},
    KeyName{Key::Insert, "Insert"},
    KeyName{Key::Minus, "Minus"},
    KeyName{Key::Equals, "Equals"},
    KeyName{Key::Comma, "Comma"},
    KeyName{Key::Period, "Period"},
    KeyName{Key::Slash, "Slash"},
    KeyName{Key::Backslash, "Backslash"},
    KeyName{Key::Semicolon, "Semicolon"},
    KeyName{Key::Apostrophe, "Apostrophe"},
    KeyName{Key::LeftBracket, "LeftBracket"},
    KeyName{Key::RightBracket, "RightBracket"},
    KeyName{Key::Grave, "Grave"},
    KeyName{Key::MouseBack, "MouseBack"},
    KeyName{Key::MouseForward, "MouseForward"},
};

struct ModifierName {
    Modifiers mod;
    std::string_view name;
};

// Canonical order of to_string(KeyChord): Super, Ctrl, Alt, Shift.
constexpr std::array kCanonicalModifiers{
    ModifierName{Modifiers::Super, "Super"},
    ModifierName{Modifiers::Ctrl, "Ctrl"},
    ModifierName{Modifiers::Alt, "Alt"},
    ModifierName{Modifiers::Shift, "Shift"},
};

// Accepted modifier spellings in parse_key_chord() (matched case-insensitively).
constexpr std::array kModifierAliases{
    ModifierName{Modifiers::Super, "Super"},  ModifierName{Modifiers::Super, "Meta"},
    ModifierName{Modifiers::Super, "Win"},    ModifierName{Modifiers::Ctrl, "Ctrl"},
    ModifierName{Modifiers::Ctrl, "Control"}, ModifierName{Modifiers::Alt, "Alt"},
    ModifierName{Modifiers::Shift, "Shift"},
};

constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

constexpr std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<Modifiers> parse_modifier(std::string_view token) noexcept {
    for (const ModifierName& m : kModifierAliases) {
        if (iequals(token, m.name)) {
            return m.mod;
        }
    }
    return std::nullopt;
}

std::optional<Key> parse_key(std::string_view token) noexcept {
    for (const KeyName& k : kKeyNames) {
        if (iequals(token, k.name)) {
            return k.key;
        }
    }
    return std::nullopt;
}

bool is_known_key(Key key) noexcept {
    return std::ranges::find(kKeyNames, key, &KeyName::key) != kKeyNames.end();
}

Unexpected<Error> chord_error(std::string_view text, std::string_view why) {
    std::string message = "invalid key chord '";
    message += text;
    message += "': ";
    message += why;
    return fail(ErrorCode::InvalidArgument, std::move(message));
}

} // namespace

std::string_view to_string(Key key) noexcept {
    const auto it = std::ranges::find(kKeyNames, key, &KeyName::key);
    return it == kKeyNames.end() ? std::string_view{"Unknown"} : it->name;
}

std::string to_string(KeyChord chord) {
    std::string out;
    for (const ModifierName& m : kCanonicalModifiers) {
        if (has(chord.mods, m.mod)) {
            out += m.name;
            out += '+';
        }
    }
    out += to_string(chord.key);
    return out;
}

Result<KeyChord> parse_key_chord(std::string_view text) {
    if (trim(text).empty()) {
        return chord_error(text, "empty");
    }
    KeyChord chord;
    bool have_key = false;
    std::string_view rest = text;
    while (true) {
        const std::size_t plus = rest.find('+');
        const std::string_view token = trim(rest.substr(0, plus));
        if (token.empty()) {
            return chord_error(text, "empty token");
        }
        if (have_key) {
            // A token after the key: either a second key or a modifier after the key.
            return chord_error(text, "the key must be the last token and appear once");
        }
        if (const std::optional<Modifiers> mod = parse_modifier(token)) {
            if (has(chord.mods, *mod)) {
                return chord_error(text, "repeated modifier");
            }
            chord.mods |= *mod;
        } else if (const std::optional<Key> key = parse_key(token)) {
            chord.key = *key;
            have_key = true;
        } else {
            return chord_error(text, "unknown token");
        }
        if (plus == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(plus + 1);
    }
    if (!have_key) {
        return chord_error(text, "missing key");
    }
    return chord;
}

Result<void> Keymap::bind(KeyChord chord, std::string_view command, std::uint64_t arg) {
    // Out-of-range Key values have no name ("Unknown") and are rejected like Key::Unknown.
    if (chord.key == Key::Unknown || !is_known_key(chord.key)) {
        return fail(ErrorCode::InvalidArgument, "cannot bind Key::Unknown");
    }
    if ((static_cast<std::uint8_t>(chord.mods) & static_cast<std::uint8_t>(~kModifierMask)) != 0U) {
        return fail(ErrorCode::InvalidArgument, "modifier bits outside kModifierMask");
    }
    if (command.empty()) {
        return fail(ErrorCode::InvalidArgument, "empty command name");
    }
    if (const KeyBinding* existing = resolve(chord)) {
        return fail(ErrorCode::AlreadyExists,
                    "chord " + to_string(chord) + " already bound to " + existing->command);
    }
    bindings_.push_back(KeyBinding{chord, std::string(command), arg});
    return ok();
}

bool Keymap::unbind(KeyChord chord) noexcept {
    const auto it = std::ranges::find(bindings_, chord, &KeyBinding::chord);
    if (it == bindings_.end()) {
        return false;
    }
    bindings_.erase(it); // vector erase of a move-assignable element: noexcept in practice
    return true;
}

void Keymap::clear() noexcept { bindings_.clear(); }

const KeyBinding* Keymap::resolve(KeyChord chord) const noexcept {
    const auto it = std::ranges::find(bindings_, chord, &KeyBinding::chord);
    return it == bindings_.end() ? nullptr : &*it;
}

std::vector<KeyChord> Keymap::chords_for(std::string_view command) const {
    std::vector<KeyChord> out;
    for (const KeyBinding& b : bindings_) {
        if (b.command == command) {
            out.push_back(b.chord);
        }
    }
    return out;
}

std::vector<KeyChord> Keymap::chords_for(std::string_view command, std::uint64_t arg) const {
    std::vector<KeyChord> out;
    for (const KeyBinding& b : bindings_) {
        if (b.command == command && b.arg == arg) {
            out.push_back(b.chord);
        }
    }
    return out;
}

Span<const KeyBinding> Keymap::bindings() const noexcept { return bindings_; }

std::optional<Command> resolve_command(const Keymap& keymap, const CommandRegistry& registry,
                                       KeyChord chord) noexcept {
    const KeyBinding* binding = keymap.resolve(chord);
    if (binding == nullptr) {
        return std::nullopt;
    }
    const CommandInfo* info = registry.find(std::string_view{binding->command});
    if (info == nullptr) {
        return std::nullopt;
    }
    return Command{info->id, binding->arg};
}

} // namespace navigator
