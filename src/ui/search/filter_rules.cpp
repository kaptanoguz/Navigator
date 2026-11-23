// src/ui/search/filter_rules.cpp — see filter_rules.hpp.
#include "search/filter_rules.hpp"

#include <algorithm>

namespace navigator::ui {

bool starts_type_to_filter(KeyboardOwner owner, Modifiers held, std::string_view text) noexcept {
    if (owner != KeyboardOwner::List || text.empty()) {
        return false;
    }
    if (has(held, Modifiers::Ctrl) || has(held, Modifiers::Alt) || has(held, Modifiers::Super)) {
        return false; // a chord, not typing (AltGr arrives as text without Alt on Linux)
    }
    if (text.front() == ' ') {
        return false; // Space stays free for the Inspector (Phase 6)
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20U || c == 0x7F) {
            return false;
        }
    }
    return true;
}

bool field_consumes_as_text(const KeyChord& chord) noexcept {
    return chord.key == Key::Space &&
           (chord.mods == Modifiers::None || chord.mods == Modifiers::Shift);
}

FilterKey route_filter_key(const KeyChord& chord, bool has_selection) noexcept {
    const Modifiers m = chord.mods;
    const bool plain = m == Modifiers::None;
    const bool shift = m == Modifiers::Shift;
    const bool ctrl = m == Modifiers::Ctrl;
    const bool ctrl_shift = m == (Modifiers::Ctrl | Modifiers::Shift);
    switch (chord.key) {
    case Key::Left:
    case Key::Right:
    case Key::Home:
    case Key::End:
        return (plain || shift || ctrl || ctrl_shift) ? FilterKey::Edit : FilterKey::Keymap;
    case Key::Backspace:
    case Key::Delete: return (plain || shift || ctrl) ? FilterKey::Edit : FilterKey::Keymap;
    case Key::V: return ctrl ? FilterKey::Edit : FilterKey::Keymap;
    case Key::Z: return (ctrl || ctrl_shift) ? FilterKey::Edit : FilterKey::Keymap;
    case Key::Y: return ctrl ? FilterKey::Edit : FilterKey::Keymap;
    case Key::C:
    case Key::X: return ctrl && has_selection ? FilterKey::Edit : FilterKey::Keymap;
    case Key::Insert: // Shift+Insert pastes, Ctrl+Insert copies (X11 habit)
        if (shift) {
            return FilterKey::Edit;
        }
        return ctrl && has_selection ? FilterKey::Edit : FilterKey::Keymap;
    case Key::Escape: return plain ? FilterKey::Escape : FilterKey::Keymap;
    case Key::Tab: return plain ? FilterKey::ToList : FilterKey::Keymap;
    default: return FilterKey::Keymap;
    }
}

std::string_view scope_label_key(SearchScope scope) noexcept {
    switch (scope) {
    case SearchScope::Folder: return {};
    case SearchScope::Subfolders: return "filter.scope_subfolders";
    case SearchScope::Drive: return "filter.scope_drive";
    }
    return {};
}

FieldEscape filter_field_escape(bool search_running, bool has_text) noexcept {
    if (search_running) {
        return FieldEscape::CancelSearch;
    }
    return has_text ? FieldEscape::ClearText : FieldEscape::ReturnFocus;
}

ListEscape list_escape_step(bool notice, bool search_running, bool selection, bool flattened,
                            bool filter_text) noexcept {
    if (notice) {
        return ListEscape::DismissNotice;
    }
    if (search_running) {
        return ListEscape::CancelSearch;
    }
    if (selection) {
        return ListEscape::ClearSelection;
    }
    if (flattened) {
        return ListEscape::LeaveFlatten;
    }
    return filter_text ? ListEscape::ClearFilter : ListEscape::Nothing;
}

TitleButtons fit_title_buttons(float room, float button, bool filter_needed, bool types_on,
                               bool flatten_on, bool star_on) noexcept {
    constexpr float kGaps = 16.0F; // around the filter
    const float base = kCrumbsMin + kGaps + (filter_needed ? kFilterFieldMin : kFilterCollapsed);
    TitleButtons out;
    const auto over = [&] {
        const int shown = (out.types ? 1 : 0) + (out.flatten ? 1 : 0) + (out.star ? 1 : 0);
        return base + (static_cast<float>(shown) * (button + 3.0F)) > room;
    };
    if (filter_needed && !star_on && over()) {
        out.star = false; // searching: flatten matters more than the bookmark star
    }
    if (!types_on && over()) {
        out.types = false;
    }
    if (!flatten_on && over()) {
        out.flatten = false;
    }
    if (!star_on && over()) {
        out.star = false;
    }
    return out;
}

FilterBarLayout layout_filter_bar(float room, float crumbs, bool needed) noexcept {
    FilterBarLayout out;
    const float keep = std::min(crumbs, kCrumbsKeep);
    if (needed) {
        // Grows into what the breadcrumbs do not need, between the minimum and the ideal; an
        // active filter keeps at least the minimum even when the breadcrumbs must elide.
        out.field = std::clamp(room - crumbs, kFilterFieldMin, kFilterFieldIdeal);
        out.field = std::max(std::min(out.field, room - keep), std::min(kFilterFieldMin, room));
        return out;
    }
    if (room - keep < kFilterFieldMin) {
        out.collapsed = true;
        out.field = std::min(kFilterCollapsed, std::max(room, 0.0F));
        return out;
    }
    out.field = std::clamp(room - crumbs, kFilterFieldMin, kFilterFieldIdeal);
    out.field = std::min(out.field, room - keep);
    return out;
}

} // namespace navigator::ui
