// src/ui/widgets/text_edit.hpp
//
// Single-line text field state (Phase 4 focus scoping, docs/keymap.md "Focus scoping"): UTF-8
// text, a caret and a selection anchor (byte offsets on code point boundaries), the editing
// operations the address bar, GoTo, bookmark rename and the Phase 3 name prompts share, and a
// small undo stack. Headless and pure: drawing and key translation live elsewhere, the
// clipboard is the caller's (cut/copy return text, paste takes it).
//
// Word motions (Ctrl+Left/Right, Ctrl+Backspace) stop at path-friendly boundaries: '/', ' ',
// '-', '_', '.' separate words, so Ctrl+Backspace in "~/Music/Albums" removes "Albums".
// Thread-safety: plain value, UI thread.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/commands.hpp>

namespace navigator::ui {

class TextEdit {
public:
    // 0: unlimited.
    explicit TextEdit(std::size_t max_bytes = 0) : max_bytes_(max_bytes) {}

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] std::size_t caret() const noexcept { return caret_; }
    [[nodiscard]] std::size_t anchor() const noexcept { return anchor_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
    [[nodiscard]] bool has_selection() const noexcept { return caret_ != anchor_; }
    // [begin, end) byte range of the selection (begin == end when none).
    [[nodiscard]] std::pair<std::size_t, std::size_t> selection() const noexcept;
    [[nodiscard]] std::string selected_text() const;
    // Increases on every change of the text (not on caret moves).
    [[nodiscard]] std::size_t revision() const noexcept { return revision_; }

    // Replaces everything (clears undo). select_all: the whole text is selected, caret at end.
    void set(std::string_view text, bool select_all = false);
    void clear() { set({}); }

    // Typing / paste: replaces the selection. Control characters (< 0x20, 0x7F) are dropped
    // (a pasted path with a trailing newline keeps working); invalid UTF-8 bytes are dropped.
    // Returns false when nothing changed (empty input, or max_bytes reached).
    bool insert(std::string_view utf8);
    // Backspace / Delete: the selection if any, else one code point (word: to the word start /
    // end). Returns false when nothing changed.
    bool backspace(bool word = false);
    bool erase_forward(bool word = false);
    // Caret motions. extend: keep the anchor (Shift); word: by word (Ctrl).
    void move_left(bool extend, bool word = false);
    void move_right(bool extend, bool word = false);
    void home(bool extend);
    void end(bool extend);
    void select_all() noexcept;
    // Cut returns the removed text (empty without a selection).
    std::string cut();
    // Reverts the last change. Returns false when there is nothing to undo.
    bool undo();

private:
    struct Snapshot {
        std::string text;
        std::size_t caret = 0;
        std::size_t anchor = 0;
    };
    void remember();
    void erase_range(std::size_t begin, std::size_t end);
    [[nodiscard]] std::size_t prev_cp(std::size_t pos) const noexcept;
    [[nodiscard]] std::size_t next_cp(std::size_t pos) const noexcept;
    [[nodiscard]] std::size_t prev_word(std::size_t pos) const noexcept;
    [[nodiscard]] std::size_t next_word(std::size_t pos) const noexcept;

    std::string text_;
    std::size_t caret_ = 0;
    std::size_t anchor_ = 0;
    std::size_t max_bytes_ = 0;
    std::size_t revision_ = 0;
    std::vector<Snapshot> undo_;
};

// Focus scoping step 1 (docs/keymap.md): the editing keys a focused field takes before the
// keymap is consulted. Left / Right / Home / End (Shift extends, Ctrl moves by word),
// Backspace / Delete (Ctrl: by word), Ctrl+A select all, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste,
// Ctrl+Z undo, and Tab (reported, the owner decides: completion). Every other chord (Enter,
// Escape, Up, Down, Ctrl+Q, F6, ...) is not handled and resolves through the keymap.
struct FieldKeyResult {
    bool handled = false;
    bool changed = false;                 // the text changed
    bool tab = false;                     // Tab was pressed (handled)
    std::optional<std::string> clipboard; // Ctrl+C / Ctrl+X: text for the clipboard
};
[[nodiscard]] FieldKeyResult apply_field_key(TextEdit& edit, const KeyChord& chord,
                                             const std::function<std::string()>& paste = {});

} // namespace navigator::ui
