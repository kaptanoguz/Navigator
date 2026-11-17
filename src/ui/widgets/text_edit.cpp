// src/ui/widgets/text_edit.cpp — see text_edit.hpp.
#include "widgets/text_edit.hpp"

#include <algorithm>

namespace navigator::ui {
namespace {

constexpr std::size_t kUndoDepth = 64;

bool is_continuation(char c) noexcept { return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U; }

bool is_separator(char c) noexcept {
    return c == '/' || c == ' ' || c == '-' || c == '_' || c == '.' || c == '\t';
}

// Length of the valid UTF-8 sequence at s[i], or 0 when it is invalid.
std::size_t utf8_length(std::string_view s, std::size_t i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    if (b0 < 0x80U) {
        return 1;
    }
    if (b0 >= 0xC2U && b0 <= 0xDFU) {
        n = 2;
    } else if (b0 >= 0xE0U && b0 <= 0xEFU) {
        n = 3;
    } else if (b0 >= 0xF0U && b0 <= 0xF4U) {
        n = 4;
    } else {
        return 0;
    }
    if (i + n > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        if (!is_continuation(s[i + k])) {
            return 0;
        }
    }
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    if ((b0 == 0xE0U && b1 < 0xA0U) || (b0 == 0xEDU && b1 > 0x9FU) || (b0 == 0xF0U && b1 < 0x90U) ||
        (b0 == 0xF4U && b1 > 0x8FU)) {
        return 0; // overlong, surrogate, or above U+10FFFF
    }
    return n;
}

// Valid UTF-8 without control characters.
std::string sanitize(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const std::size_t n = utf8_length(in, i);
        if (n == 0) {
            ++i;
            continue;
        }
        const auto c = static_cast<unsigned char>(in[i]);
        if (n == 1 && (c < 0x20U || c == 0x7FU)) {
            ++i;
            continue;
        }
        out.append(in.substr(i, n));
        i += n;
    }
    return out;
}

} // namespace

std::pair<std::size_t, std::size_t> TextEdit::selection() const noexcept {
    return {std::min(caret_, anchor_), std::max(caret_, anchor_)};
}

std::string TextEdit::selected_text() const {
    const auto [b, e] = selection();
    return text_.substr(b, e - b);
}

void TextEdit::set(std::string_view text, bool select_all) {
    text_ = sanitize(text);
    if (max_bytes_ > 0 && text_.size() > max_bytes_) {
        std::size_t cut = max_bytes_;
        while (cut > 0 && is_continuation(text_[cut])) {
            --cut;
        }
        text_.resize(cut);
    }
    caret_ = text_.size();
    anchor_ = select_all ? 0 : caret_;
    undo_.clear();
    ++revision_;
}

void TextEdit::remember() {
    if (undo_.size() >= kUndoDepth) {
        undo_.erase(undo_.begin());
    }
    undo_.push_back(Snapshot{text_, caret_, anchor_});
}

void TextEdit::erase_range(std::size_t begin, std::size_t end) {
    text_.erase(begin, end - begin);
    caret_ = begin;
    anchor_ = begin;
    ++revision_;
}

bool TextEdit::insert(std::string_view utf8) {
    std::string clean = sanitize(utf8);
    if (clean.empty()) {
        return false;
    }
    const auto [b, e] = selection();
    if (max_bytes_ > 0) {
        const std::size_t room = max_bytes_ - std::min(max_bytes_, text_.size() - (e - b));
        if (room == 0) {
            return false;
        }
        if (clean.size() > room) {
            std::size_t cut = room;
            while (cut > 0 && is_continuation(clean[cut])) {
                --cut;
            }
            clean.resize(cut);
            if (clean.empty()) {
                return false;
            }
        }
    }
    remember();
    text_.replace(b, e - b, clean);
    caret_ = b + clean.size();
    anchor_ = caret_;
    ++revision_;
    return true;
}

bool TextEdit::backspace(bool word) {
    if (has_selection()) {
        remember();
        const auto [b, e] = selection();
        erase_range(b, e);
        return true;
    }
    if (caret_ == 0) {
        return false;
    }
    remember();
    const std::size_t from = word ? prev_word(caret_) : prev_cp(caret_);
    erase_range(from, caret_);
    return true;
}

bool TextEdit::erase_forward(bool word) {
    if (has_selection()) {
        remember();
        const auto [b, e] = selection();
        erase_range(b, e);
        return true;
    }
    if (caret_ >= text_.size()) {
        return false;
    }
    remember();
    const std::size_t to = word ? next_word(caret_) : next_cp(caret_);
    erase_range(caret_, to);
    return true;
}

void TextEdit::move_left(bool extend, bool word) {
    if (!extend && has_selection() && !word) {
        caret_ = selection().first;
        anchor_ = caret_;
        return;
    }
    caret_ = word ? prev_word(caret_) : prev_cp(caret_);
    if (!extend) {
        anchor_ = caret_;
    }
}

void TextEdit::move_right(bool extend, bool word) {
    if (!extend && has_selection() && !word) {
        caret_ = selection().second;
        anchor_ = caret_;
        return;
    }
    caret_ = word ? next_word(caret_) : next_cp(caret_);
    if (!extend) {
        anchor_ = caret_;
    }
}

void TextEdit::home(bool extend) {
    caret_ = 0;
    if (!extend) {
        anchor_ = 0;
    }
}

void TextEdit::end(bool extend) {
    caret_ = text_.size();
    if (!extend) {
        anchor_ = caret_;
    }
}

void TextEdit::select_all() noexcept {
    anchor_ = 0;
    caret_ = text_.size();
}

std::string TextEdit::cut() {
    if (!has_selection()) {
        return {};
    }
    std::string out = selected_text();
    remember();
    const auto [b, e] = selection();
    erase_range(b, e);
    return out;
}

bool TextEdit::undo() {
    if (undo_.empty()) {
        return false;
    }
    Snapshot s = std::move(undo_.back());
    undo_.pop_back();
    text_ = std::move(s.text);
    caret_ = std::min(s.caret, text_.size());
    anchor_ = std::min(s.anchor, text_.size());
    ++revision_;
    return true;
}

std::size_t TextEdit::prev_cp(std::size_t pos) const noexcept {
    if (pos == 0) {
        return 0;
    }
    --pos;
    while (pos > 0 && is_continuation(text_[pos])) {
        --pos;
    }
    return pos;
}

std::size_t TextEdit::next_cp(std::size_t pos) const noexcept {
    if (pos >= text_.size()) {
        return text_.size();
    }
    ++pos;
    while (pos < text_.size() && is_continuation(text_[pos])) {
        ++pos;
    }
    return pos;
}

std::size_t TextEdit::prev_word(std::size_t pos) const noexcept {
    // Skip separators left of the caret, then the word itself.
    while (pos > 0 && is_separator(text_[pos - 1])) {
        --pos;
    }
    while (pos > 0 && !is_separator(text_[pos - 1])) {
        --pos;
    }
    return pos;
}

std::size_t TextEdit::next_word(std::size_t pos) const noexcept {
    const std::size_t n = text_.size();
    while (pos < n && is_separator(text_[pos])) {
        ++pos;
    }
    while (pos < n && !is_separator(text_[pos])) {
        ++pos;
    }
    return pos;
}

FieldKeyResult apply_field_key(TextEdit& edit, const KeyChord& chord,
                               const std::function<std::string()>& paste) {
    FieldKeyResult r;
    const Modifiers m = chord.mods;
    const bool ctrl = has(m, Modifiers::Ctrl);
    const bool shift = has(m, Modifiers::Shift);
    if (has(m, Modifiers::Alt) || has(m, Modifiers::Super)) {
        return r; // Alt+Left (nav.back) and friends stay commands
    }
    const std::size_t rev = edit.revision();
    switch (chord.key) {
    case Key::Left:
        edit.move_left(shift, ctrl);
        r.handled = true;
        break;
    case Key::Right:
        edit.move_right(shift, ctrl);
        r.handled = true;
        break;
    case Key::Home:
        if (!ctrl) {
            edit.home(shift);
            r.handled = true;
        }
        break;
    case Key::End:
        if (!ctrl) {
            edit.end(shift);
            r.handled = true;
        }
        break;
    case Key::Backspace:
        if (!shift) {
            (void)edit.backspace(ctrl);
            r.handled = true;
        }
        break;
    case Key::Delete:
        if (!shift) {
            (void)edit.erase_forward(ctrl);
            r.handled = true;
        }
        break;
    case Key::Tab:
        if (m == Modifiers::None) {
            r.handled = true;
            r.tab = true;
        }
        break;
    case Key::A:
        if (ctrl && !shift) {
            edit.select_all();
            r.handled = true;
        }
        break;
    case Key::C:
        if (ctrl && !shift) {
            r.handled = true;
            if (edit.has_selection()) {
                r.clipboard = edit.selected_text();
            }
        }
        break;
    case Key::X:
        if (ctrl && !shift) {
            r.handled = true;
            if (edit.has_selection()) {
                r.clipboard = edit.cut();
            }
        }
        break;
    case Key::V:
        if (ctrl && !shift) {
            r.handled = true;
            if (paste) {
                (void)edit.insert(paste());
            }
        }
        break;
    case Key::Z:
        if (ctrl && !shift) {
            (void)edit.undo();
            r.handled = true;
        }
        break;
    default: break;
    }
    r.changed = edit.revision() != rev;
    return r;
}

} // namespace navigator::ui
