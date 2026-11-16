// navigator/commands.hpp
//
// Purpose: command identifiers, the per-frame command queue, the command registry that the
//          keymap and command palette are built on, and the keymap itself (key chords ->
//          command names). No SDL types: the ui layer translates SDL events into Key/Modifiers.
// Owner:   contracts (header); implementation in src/commands (role: commands).
// Thread-safety: NOT thread-safe. CommandQueue, CommandRegistry and Keymap are owned and used by
//                the UI thread only. Worker threads report back through their own channels and
//                the UI thread turns results into commands. The constexpr helpers (Modifiers
//                operators, KeyChord comparison) and to_string()/parse_key_chord() are pure and
//                may be called from any thread.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
//
// Frame model: widgets never mutate program state while drawing. A user action calls
// CommandQueue::enqueue(); at frame end the app calls drain() and runs every pending command in
// FIFO order. Commands enqueued while a drain is running (by a handler) are NOT run by that drain;
// they stay pending and run on the next drain (normally the next frame).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

// Stable numeric ids: never renumber or reuse a value; append new ids. Values may be persisted
// (keymap files, plugins). Ranges:
//      1- 999  app / view
//   1000-1099  navigation (history, location)
//   1100-1199  list: activation, cursor, selection
//   1200+      reserved for later phases (tabs, panes, file operations, search, ...)
// Every id except None is registered by register_builtin_commands() and appears in the palette.
// `arg` meanings are listed per id; "arg: 0" means the command ignores it.
enum class CommandId : std::uint32_t {
    None = 0,             // "none"       no-op; never registered
    Quit = 1,             // "app.quit"
    ToggleFpsOverlay = 2, // "view.toggle_fps_overlay"
    ToggleTheme = 3,      // "view.toggle_theme"
    ToggleHidden = 4,     // "view.toggle_hidden"    show/hide dot-files; re-lists. arg: 0

    // Navigation. arg: 0 for all.
    NavigateBack = 1000,    // "nav.back"     previous history location (no-op at the start)
    NavigateForward = 1001, // "nav.forward"  next history location (no-op at the end)
    NavigateUp = 1002,      // "nav.up"       parent directory (no-op at "/")
    Refresh = 1003,         // "nav.refresh"  re-list the current directory
    FocusAddressBar = 1004, // "nav.focus_address_bar"  Phase 4; registered, unbound, no-op

    // List. Cursor commands take the CursorArg bit flags below as `arg`.
    Activate = 1100,       // "list.activate"  open the cursor entry: folder (or link to a folder)
                           //                  -> enter it; otherwise platform::open_with_default.
                           //                  arg: 0
    SelectAll = 1101,      // "list.select_all"       arg: 0
    ClearSelection = 1102, // "list.clear_selection"  arg: 0 (cursor stays)
    CursorUp = 1110,       // "list.cursor_up"        one row up
    CursorDown = 1111,     // "list.cursor_down"      one row down
    CursorPageUp = 1112,   // "list.cursor_page_up"   one visible page up
    CursorPageDown = 1113, // "list.cursor_page_down" one visible page down
    CursorHome = 1114,     // "list.cursor_home"      first row
    CursorEnd = 1115,      // "list.cursor_end"       last row
};

// Bit flags for the `arg` of CursorUp..CursorEnd. Unknown bits are ignored (reserved).
//   0 (no flags): move the cursor; the selection becomes exactly the new cursor row and the
//                 selection anchor moves to it.
//   Extend:       select the contiguous range anchor..new cursor (Shift+arrow); the anchor stays.
// Later phases may add bits (e.g. "move cursor without touching the selection" for the File
// Pilot-style Ctrl+Space cursor selection, Phase 8) without new ids.
struct CursorArg {
    static constexpr std::uint64_t None = 0;
    static constexpr std::uint64_t Extend = std::uint64_t{1} << 0U;
};

// A queued command. `arg` is a command-specific payload; 0 when unused. Commands that need more
// data than one integer will carry an index/handle into UI-owned state (decided per command).
struct Command {
    CommandId id = CommandId::None;
    std::uint64_t arg = 0;

    friend constexpr bool operator==(const Command&, const Command&) = default;
};

class CommandQueue {
public:
    CommandQueue() = default;

    // Appends to the pending list. Enqueuing CommandId::None is allowed and is a no-op when run.
    void enqueue(Command command);
    void enqueue(CommandId id, std::uint64_t arg = 0) { enqueue(Command{id, arg}); }

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    // Moves all pending commands out (FIFO order) and leaves the queue empty.
    [[nodiscard]] std::vector<Command> take();

    // Calls handler(const Command&) for every command pending at the moment of the call, in FIFO
    // order. Commands the handler enqueues are kept for the next drain.
    template <class F> void drain(F&& handler) {
        const std::vector<Command> batch = take();
        for (const Command& command : batch) {
            handler(command);
        }
    }

private:
    std::vector<Command> pending_;
};

// Static description of a command.
//   name:      stable dotted id, e.g. "app.quit" (keymap files and the palette use it)
//   title_key: i18n key for the palette title, e.g. "cmd.app.quit" (see i18n.hpp)
// Both views must point to storage that outlives the registry (string literals in practice).
struct CommandInfo {
    CommandId id = CommandId::None;
    std::string_view name;
    std::string_view title_key;
};

class CommandRegistry {
public:
    CommandRegistry() = default;

    // Errors: InvalidArgument (id None or empty name), AlreadyExists (duplicate id or name).
    Result<void> add(const CommandInfo& info);

    // nullptr when not registered. Pointers stay valid until the next add().
    [[nodiscard]] const CommandInfo* find(CommandId id) const noexcept;
    [[nodiscard]] const CommandInfo* find(std::string_view name) const noexcept;

    // All commands in registration order. Valid until the next add().
    [[nodiscard]] Span<const CommandInfo> all() const noexcept;

private:
    std::vector<CommandInfo> commands_;
};

// Registers every built-in CommandId (except None) with its name and title_key
// ("cmd." + name). Registration order: ascending CommandId. A failure (AlreadyExists) leaves
// the registry unchanged.
Result<void> register_builtin_commands(CommandRegistry& registry);

// Stable name of a built-in id ("app.quit", ...); "none" for None, "unknown" for other values.
[[nodiscard]] std::string_view to_string(CommandId id) noexcept;

// =============================================================================================
// Keymap: key chords -> command names
// =============================================================================================
//
// Translation from the windowing system (done in src/ui, never here):
//   - Keys match by keycode, i.e. the legend the active layout puts on the key, so Ctrl+Q is
//     the key labelled Q on Turkish Q, Turkish F and US layouts alike. ASCII letters are folded
//     to lowercase first (with Shift held SDL may report the uppercase legend).
//   - If the keycode is not a Latin letter (Turkish ı ş ğ ü ö ç, Cyrillic, Greek, ...), the ui
//     falls back to the physical key position (scancode) so that Ctrl+<that key> still resolves
//     to the US-position letter. A Latin legend that has no Key value does NOT fall back (it
//     must not steal the binding of the letter that sits at its position on a US layout).
//   - Left/right modifiers are merged. CapsLock, NumLock and AltGr (ISO_Level3_Shift,
//     SDL_KMOD_MODE) are never reported as modifiers; AltGr is not Alt.
//   - Keypad digits/operators map to Key::Unknown in Phase 2; only KeypadEnter is distinct.
//   - Mouse side buttons (X1/X2) map to Key::MouseBack / Key::MouseForward so they bind through
//     the same keymap. Clicks, double-clicks and drags are not chords (see docs/architecture.md).
//
// Phase 2 bindings are global (no focus contexts). Focus-scoped bindings arrive with the address
// bar (Phase 4). Multi-key sequences ("g g") and command aliases arrive in Phase 8 as an addition
// (e.g. a KeySequence type and Keymap::bind_sequence); the single-chord API below stays.

// Numeric values are not persisted anywhere (keymap files use to_string() names); they may be
// regrouped between versions. Append within a group when possible.
// clang-format off
enum class Key : std::uint16_t {
    Unknown = 0,

    // Letters (US-position legend), 1..26
    A = 1, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Top-row digits, 40..49
    Digit0 = 40, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,

    // Function keys, 60..83
    F1 = 60, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,

    // Navigation and editing, 100..115
    Up = 100, Down, Left, Right, PageUp, PageDown, Home, End,
    Enter, KeypadEnter, Escape, Backspace, Delete, Tab, Space, Insert,

    // Punctuation by US legend, 140..150 (for later bindings such as Ctrl+, and Ctrl+=)
    Minus = 140, Equals, Comma, Period, Slash, Backslash, Semicolon, Apostrophe, LeftBracket,
    RightBracket, Grave,

    // Mouse side buttons (X1 / X2), 200..201
    MouseBack = 200, MouseForward,
};
// clang-format on

// Bit set of held modifiers.
enum class Modifiers : std::uint8_t {
    None = 0,
    Ctrl = 1U << 0U,
    Shift = 1U << 1U,
    Alt = 1U << 2U,
    Super = 1U << 3U,
};

inline constexpr std::uint8_t kModifierMask = 0x0FU; // all defined Modifiers bits

[[nodiscard]] constexpr Modifiers operator|(Modifiers a, Modifiers b) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr Modifiers operator&(Modifiers a, Modifiers b) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr Modifiers operator^(Modifiers a, Modifiers b) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) ^ static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr Modifiers operator~(Modifiers a) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(a)) &
                                  kModifierMask);
}
constexpr Modifiers& operator|=(Modifiers& a, Modifiers b) noexcept { return a = a | b; }
constexpr Modifiers& operator&=(Modifiers& a, Modifiers b) noexcept { return a = a & b; }
constexpr Modifiers& operator^=(Modifiers& a, Modifiers b) noexcept { return a = a ^ b; }

// True when every bit of `flags` is set in `set` (has(x, Modifiers::None) is true).
[[nodiscard]] constexpr bool has(Modifiers set, Modifiers flags) noexcept {
    return (set & flags) == flags;
}

// One key press with the exact set of modifiers held. Matching is exact: Ctrl+A does not fire
// for Ctrl+Shift+A.
struct KeyChord {
    Key key = Key::Unknown;
    Modifiers mods = Modifiers::None;

    friend constexpr bool operator==(const KeyChord&, const KeyChord&) = default;
};

// Canonical ASCII name of a key, identical in every UI language: "A".."Z", "0".."9", "F1".."F24",
// "Up", "Down", "Left", "Right", "PageUp", "PageDown", "Home", "End", "Enter", "KeypadEnter",
// "Escape", "Backspace", "Delete", "Tab", "Space", "Insert", "Minus", "Equals", "Comma",
// "Period", "Slash", "Backslash", "Semicolon", "Apostrophe", "LeftBracket", "RightBracket",
// "Grave", "MouseBack", "MouseForward"; "Unknown" for Key::Unknown and out-of-range values.
[[nodiscard]] std::string_view to_string(Key key) noexcept;

// Canonical ASCII chord text: modifiers in the order Super, Ctrl, Alt, Shift, then the key, joined
// by '+': "Ctrl+Shift+L", "Alt+Left", "F5". This is the keymap-file / log / test form. The ui
// renders key hints from it with translated modifier names (i18n keys "key.ctrl", "key.shift",
// "key.alt", "key.super"; key legends themselves are not translated).
[[nodiscard]] std::string to_string(KeyChord chord);

// Inverse of to_string(KeyChord). Tokens are separated by '+', matched ASCII-case-insensitively,
// surrounding spaces ignored; modifiers may appear in any order and also as "Control" and
// "Meta"/"Win" (= Super). Exactly one key token, last. Errors: InvalidArgument (empty text, empty
// token, unknown token, repeated modifier, missing key, more than one key, "Unknown").
[[nodiscard]] Result<KeyChord> parse_key_chord(std::string_view text);

// A chord bound to a command name with an optional arg (see CommandId comments), e.g.
// Shift+Down -> {"list.cursor_down", CursorArg::Extend}.
struct KeyBinding {
    KeyChord chord;
    std::string command; // CommandInfo::name; not validated against a registry by Keymap
    std::uint64_t arg = 0;
};

class Keymap {
public:
    Keymap() = default;

    // Adds a binding. A chord maps to at most one binding; one command may have many chords.
    // Errors: InvalidArgument (chord.key == Key::Unknown, mods outside kModifierMask, or empty
    // command), AlreadyExists (the chord is already bound, to anything; the map is unchanged).
    Result<void> bind(KeyChord chord, std::string_view command, std::uint64_t arg = 0);

    // Removes the binding of `chord`. Returns false when the chord was not bound.
    bool unbind(KeyChord chord) noexcept;

    // Removes every binding.
    void clear() noexcept;

    // Binding for the exact chord, or nullptr. Valid until the next bind/unbind/clear.
    [[nodiscard]] const KeyBinding* resolve(KeyChord chord) const noexcept;

    // Chords bound to `command` (any arg), in bind order. The first one is the "primary" chord
    // shown as the key hint in menus and the palette.
    [[nodiscard]] std::vector<KeyChord> chords_for(std::string_view command) const;

    // Chords bound to exactly (command, arg), in bind order.
    [[nodiscard]] std::vector<KeyChord> chords_for(std::string_view command,
                                                   std::uint64_t arg) const;

    // All bindings in bind order. Valid until the next bind/unbind/clear.
    [[nodiscard]] Span<const KeyBinding> bindings() const noexcept;

private:
    std::vector<KeyBinding> bindings_;
};

// Adds Navigator's default bindings (docs/keymap.md, in the table order there). Binds into the
// given map without clearing it. Errors: AlreadyExists if any default chord is already bound; in
// that case nothing is added.
Result<void> load_default_keymap(Keymap& keymap);

// Resolves a chord through the keymap and the registry: the Command {id, arg} to enqueue, or
// nullopt when the chord is unbound or its command name is not registered.
[[nodiscard]] std::optional<Command>
resolve_command(const Keymap& keymap, const CommandRegistry& registry, KeyChord chord) noexcept;

} // namespace navigator
