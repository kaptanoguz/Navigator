// src/ui/search/filter_rules.hpp
//
// Phase 5 keyboard and layout rules of the folder filter, pure so tests pin them down
// (docs/keymap.md "Phase 5 defaults", "Type to filter", "Focus scoping (Phase 5)"; design
// review Phase 4 "Notes for Phase 5").
//   - Type to filter: SDL text input while the pane's LIST has the keyboard and no Ctrl / Alt /
//     Super is held starts the filter, except a leading Space (reserved for the Inspector).
//   - The focused filter field consumes FEWER keys than the Phase 4 fields: editing keys only;
//     Up/Down/Page keys, Enter, Ctrl+A, Ctrl+E and every other chord go through the keymap to
//     the active pane. Ctrl+C / Ctrl+X are the field's only while it has a text selection.
//   - Escape ladders: the field (cancel walk -> clear text -> back to the list) and the list
//     (notice -> running search -> selection -> flattened view -> filter text).
//   - Title bar width rule: the field shrinks to a minimum, then collapses to an icon button
//     before it squeezes the breadcrumbs; an active filter never collapses.
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <string_view>

#include <navigator/commands.hpp>

#include "sidebar/sidebar_model.hpp"

namespace navigator::ui {

// Does this text input start (or extend) the filter of the pane whose list has the keyboard?
[[nodiscard]] bool starts_type_to_filter(KeyboardOwner owner, Modifiers held,
                                         std::string_view text) noexcept;

enum class FilterKey : std::uint8_t {
    Edit,   // the field's editing key: apply_field_key()
    Escape, // the field's Escape ladder (filter_field_escape)
    ToList, // Tab: focus returns to the list, the filter stays applied
    Keymap, // not the field's: resolve through the keymap (to the active pane)
};
[[nodiscard]] FilterKey route_filter_key(const KeyChord& chord, bool has_selection) noexcept;

enum class FieldEscape : std::uint8_t { CancelSearch, ClearText, ReturnFocus };
[[nodiscard]] FieldEscape filter_field_escape(bool search_running, bool has_text) noexcept;

enum class ListEscape : std::uint8_t {
    DismissNotice,
    CancelSearch,
    ClearSelection,
    LeaveFlatten,
    ClearFilter,
    Nothing,
};
[[nodiscard]] ListEscape list_escape_step(bool notice, bool search_running, bool selection,
                                          bool flattened, bool filter_text) noexcept;

// Title bar: the filter field between the breadcrumbs and the right-hand buttons (dp).
inline constexpr float kFilterFieldMin = 140.0F;
inline constexpr float kFilterFieldIdeal = 224.0F;
inline constexpr float kFilterCollapsed = 30.0F; // the icon button
inline constexpr float kCrumbsKeep = 120.0F;     // breadcrumbs keep at least this much
struct FilterBarLayout {
    float field = 0.0F; // width of the field (or the collapsed button)
    bool collapsed = false;
    friend constexpr bool operator==(const FilterBarLayout&, const FilterBarLayout&) = default;
};
// `room`: width between the Up button and the right-hand buttons; `crumbs`: the breadcrumbs'
// natural width; `needed`: the filter has text or focus (never collapses).
[[nodiscard]] FilterBarLayout layout_filter_bar(float room, float crumbs, bool needed) noexcept;

// Narrow panes: which right-hand buttons still fit. The current folder's name and the filter
// (its button, or the field at its minimum while active) come first; then, as room runs out,
// types, flatten and the bookmark star hide (the star first while the filter is in use, so
// flatten stays beside the query), unless they show an active state. Their commands and
// chords keep working.
inline constexpr float kCrumbsMin = 60.0F; // the current folder's name, elided
struct TitleButtons {
    bool types = true;
    bool flatten = true;
    bool star = true;
    friend constexpr bool operator==(const TitleButtons&, const TitleButtons&) = default;
};
// `room`: from the Up button to the right margin; `button`: one button's width (dp).
[[nodiscard]] TitleButtons fit_title_buttons(float room, float button, bool filter_needed,
                                             bool types_on, bool flatten_on, bool star_on) noexcept;

} // namespace navigator::ui
