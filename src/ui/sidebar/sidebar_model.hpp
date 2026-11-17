// src/ui/sidebar/sidebar_model.hpp
//
// The sidebar's rows, rebuilt at frame start from the PlacesModel, the window's SidebarState and
// the active pane's folder (docs/architecture.md "Sidebar model"). Pure and headless: the view
// draws these rows, the App routes keyboard focus over them.
//
// Sections, top to bottom: Bookmarks, Recents (newest first, at most kMaxRecentRows), Places
// (Home, the XDG user dirs that exist, File System), Storage (every volume except Root, Home and
// Virtual ones; Root's usage is drawn on File System, Home's on Home). Each section starts
// with a header row (collapsible); a collapsed section has no item rows; an empty Bookmarks or
// Recents section shows one Empty row (hint text). Places whose folder the probe found missing
// are left out (a configured but deleted XDG dir); bookmarks and recents never are: they show
// Offline (volume not mounted) or Missing.
//
// Highlight: places::deepest_containing(active folder, effective paths of the visible
// bookmark, place and volume rows that are Online); only the winner is highlighted.
// Thread-safety: plain values and pure functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <navigator/layout.hpp>
#include <navigator/places.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>

#include "sidebar/places_model.hpp"

namespace navigator::ui {

inline constexpr std::size_t kMaxRecentRows = 8;

enum class SidebarRowKind : std::uint8_t {
    Header,
    Bookmark,
    Recent,
    Place,
    Volume,
    Empty, // hint row of an empty Bookmarks / Recents section (not focusable)
};

struct SidebarRow {
    SidebarRowKind kind = SidebarRowKind::Place;
    layout::SidebarSection section = layout::SidebarSection::Places;
    std::string label;  // UTF-8, already translated / derived
    Path path;          // effective path (navigation target); empty for headers
    Path stored_path;   // bookmarks / recents: the stored path (tooltips, offline notice)
    std::string source; // network volumes: the mount source ("nas.local:/export/media")
    places::LocationStatus status = places::LocationStatus::Online;
    places::PlaceKind place = places::PlaceKind::Home; // Place rows
    places::BookmarkId bookmark;                       // Bookmark rows
    // Volume rows, and Home / File System places (their volume's usage).
    std::optional<std::size_t> volume; // index into PlacesModel::volumes()
    VolumeKind volume_kind = VolumeKind::Other;
    bool removable = false;
    bool network = false;
    bool read_only = false;
    std::optional<double> usage; // used_fraction when the volume has space info
    std::uint64_t total_bytes = 0;
    std::uint64_t available_bytes = 0;
    // Header rows.
    bool collapsed = false;
    std::size_t count = 0;     // items in the section (Recents: all entries, not just shown ones)
    bool tracking_off = false; // Recents header / empty row: RecentsOptions::enabled == false
    bool highlighted = false;  // contains the active pane's folder (one row at most)

    [[nodiscard]] bool focusable() const noexcept { return kind != SidebarRowKind::Empty; }
    [[nodiscard]] bool item() const noexcept {
        return kind != SidebarRowKind::Header && kind != SidebarRowKind::Empty;
    }
    [[nodiscard]] bool online() const noexcept { return status == places::LocationStatus::Online; }
};

// Display name of a volume: its label; else, for a network share, "media on nas.local" from
// "nas.local:/export/media" or "//nas.local/media" (GNOME convention, design review O6; the
// raw device when it has neither form); else places::default_label(mount_point).
[[nodiscard]] std::string volume_label(const VolumeInfo& volume);

// The rows for one frame. `active_folder` may be empty (nothing highlighted).
[[nodiscard]] std::vector<SidebarRow> build_sidebar_rows(const PlacesModel& model,
                                                         const layout::SidebarState& state,
                                                         const Path& active_folder,
                                                         std::size_t max_recents = kMaxRecentRows);

// Keyboard focus over rows: the next focusable row from `from` by `delta` (clamped, no wrap);
// from == npos starts before the first / after the last row. npos when there is none.
[[nodiscard]] std::size_t sidebar_focus_step(const std::vector<SidebarRow>& rows, std::size_t from,
                                             std::ptrdiff_t delta);
// The highlighted row, else the first focusable row, else npos.
[[nodiscard]] std::size_t sidebar_initial_focus(const std::vector<SidebarRow>& rows);
// Row of a bookmark, or npos.
[[nodiscard]] std::size_t sidebar_row_of(const std::vector<SidebarRow>& rows,
                                         places::BookmarkId id);
// Insertion index among the bookmarks for a drop before row `row` (row of the Bookmarks
// section, or the header / empty row: index 0, or past the last bookmark: size).
[[nodiscard]] std::size_t sidebar_bookmark_index(const std::vector<SidebarRow>& rows,
                                                 std::size_t row);

// pane.focus_next / pane.focus_previous (F6 / Shift+F6) with the sidebar as one more stop:
// sidebar -> panes in reading order -> sidebar (docs/keymap.md, Phase 4). Without a visible
// sidebar it is the Phase 3 pane cycle. delta: +1 next, -1 previous; wraps.
struct FocusStop {
    bool sidebar = false;
    std::size_t pane = 0; // index in pane order (when !sidebar)
    friend constexpr bool operator==(const FocusStop&, const FocusStop&) = default;
};
[[nodiscard]] FocusStop cycle_focus(std::size_t pane_count, bool sidebar_visible,
                                    bool sidebar_focused, std::size_t active_pane, int delta);

// Who receives the keyboard (design review Phase 4 B1): exactly one place shows a focus ring.
// A popup (GoTo, Search Tabs, menus) wins, then a focused text field (address bar, bookmark
// rename), then the sidebar; otherwise the active pane's list. Only List draws the list cursor
// ring and the focused selection colours; the active pane keeps its active marker either way.
enum class KeyboardOwner : std::uint8_t { List, Sidebar, TextField, Popup };
[[nodiscard]] constexpr KeyboardOwner keyboard_owner(bool popup_open, bool text_field,
                                                     bool sidebar_focused) noexcept {
    if (popup_open) {
        return KeyboardOwner::Popup;
    }
    if (text_field) {
        return KeyboardOwner::TextField;
    }
    return sidebar_focused ? KeyboardOwner::Sidebar : KeyboardOwner::List;
}

// Narrow windows collapse the sidebar to icons (design review, Phase 3 notes): below this
// window width (dp) the sidebar is icon-only. Derived every frame, never persisted.
inline constexpr float kSidebarIconOnlyBelow = 1000.0F;
[[nodiscard]] constexpr bool sidebar_icon_only(float window_width_dp) noexcept {
    return window_width_dp < kSidebarIconOnlyBelow;
}

} // namespace navigator::ui
