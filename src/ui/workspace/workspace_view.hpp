// src/ui/workspace/workspace_view.hpp
//
// The window's content, immediate mode (Phase 3): one BrowserView per tab inside its pane, a tab
// strip per pane, splitters, the tab drag-and-drop feedback (insertion marker, split preview,
// floating tab), the window-level theme button, the Search Tabs / layout popups, the tab context
// menu, toasts and the FPS overlay.
//
// Contract with the App (docs/architecture.md, frame model and "Phase 3"):
//   update(frame)  before drawing: pane geometry, strip layout, the visible views' update().
//   draw(frame)    reads the Workspace; records Intents (pointer gestures that carry ids,
//                  positions or text) and enqueues commands. Never mutates the Workspace.
// Pointer precedence: an open popup or menu takes the pointer; then a tab or splitter drag;
// then the tab strips and splitters; the browser views get the pointer only when none of those
// claims it. Any press inside an inactive pane first records ActivatePane.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/layout.hpp>
#include <navigator/places.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

#include "browser/browser_view.hpp"
#include "path/path_entry.hpp"
#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/text_fit.hpp"
#include "widgets/ui_context.hpp"
#include "workspace/drag_drop.hpp"
#include "workspace/pane_geometry.hpp"
#include "workspace/tab_search.hpp"
#include "workspace/tab_strip.hpp"
#include "workspace/workspace.hpp"

namespace navigator::ui {

struct ShellFrame {
    const Painter& painter;
    const Palette& palette;
    Theme theme;
    const Workspace& workspace;
    const Keymap& keymap;
    const PointerInput& pointer;
    CommandQueue& queue;
    std::vector<Intent>& intents;
    float width = 0.0F; // px
    float height = 0.0F;
    bool focused = true; // window has keyboard focus
    bool animations = true;
    Density density = Density::Compact;
    double dt_ms = 16.0;
    double now_ms = 0.0; // monotonic, for toasts
    Timestamp now{};
    const Path* home = nullptr;
    bool show_fps = false;
    double fps = 0.0;
    double frame_ms = 0.0;
    std::string_view render_driver;
    // Phase 4: the panes' area (the sidebar takes the rest of the window), the bookmark store
    // (address bar star), and whether the sidebar shows (it then hosts the theme switch).
    RectF content;
    const places::BookmarkStore* bookmarks = nullptr;
    bool sidebar_visible = false;
    // Called by draw(): the sidebar right after the background, its overlays (tooltips) after
    // the panes and before toasts and popups (which cover it with their scrim).
    std::function<void()> draw_sidebar;
    std::function<void()> draw_sidebar_overlay;
};

enum class PointerCursor : std::uint8_t { Default, ResizeHorizontal, ResizeVertical };

class WorkspaceView {
public:
    // Layout metrics (dp) shared with tests.
    static constexpr float kTabMinWidth = 92.0F;
    static constexpr float kTabMaxWidth = 220.0F;
    static constexpr float kTabGap = 2.0F;
    static constexpr float kStripButton = 26.0F;
    static constexpr float kMinPaneWidth = 240.0F;
    static constexpr float kMinPaneHeight = 180.0F;
    static constexpr float kSplitterHit = 8.0F;
    static constexpr float kDragThreshold = 4.0F;

    WorkspaceView() = default;

    void update(const ShellFrame& frame);
    void draw(const ShellFrame& frame);

    // Smooth scroll, a drag, a fade or a toast is running: render continuously.
    [[nodiscard]] bool animating() const noexcept;
    [[nodiscard]] PointerCursor cursor() const noexcept { return cursor_; }

    // Per-tab views (created on demand). Views of tabs that left the layout, and every view
    // when the layout object was replaced (Workspace::layout_epoch), are dropped by prune().
    [[nodiscard]] BrowserView& view(layout::TabId tab);
    [[nodiscard]] const BrowserView* find_view(layout::TabId tab) const;
    void prune(const Workspace& ws);
    void clear_caches() noexcept;

    // ---- Popups (docs/keymap.md "Popups") ---------------------------------------------------
    [[nodiscard]] bool popup_open() const noexcept { return popup_.kind != PopupKind::None; }
    [[nodiscard]] bool wants_text_input() const noexcept;
    void open_search_tabs(const Workspace& ws, const Path* home);
    void open_save_layout(const Workspace& ws);
    void open_layout_picker(const Workspace& ws, bool delete_mode);
    void close_popup() noexcept;
    // Routes a command while a popup is open. Returns true when the popup consumed it (every
    // command except app.quit, view.toggle_theme and view.toggle_fps_overlay).
    bool route_command(const Command& command, CommandQueue& queue, std::vector<Intent>& out);
    void text_input(std::string_view utf8);
    // Delete in the layout picker: asks, then deletes the highlighted layout (two steps, like
    // its delete button). Returns false when the open popup is not the picker.
    bool delete_key(std::vector<Intent>& out);
    // Menu / Shift+F10: opens the tab context menu of the active tab on the next frame.
    void request_tab_menu() noexcept { menu_request_ = true; }
    void backspace();

    // ---- Phase 4: text fields, GoTo, address bar, context menus ------------------------------
    // The focused text field (popup query, GoTo box, address bar), or nullptr. The App edits it
    // with the editing keys (docs/keymap.md "Focus scoping") and then calls field_changed().
    [[nodiscard]] TextEdit* active_field() noexcept;
    void field_changed(double now_ms);
    // Tab in a field: accept the highlighted completion. False when the field has none.
    bool field_tab(double now_ms);
    void attach_paths(PathWorker* worker) noexcept;
    void open_goto(const Workspace& ws, const Path& home, std::vector<GotoSource> sources,
                   double now_ms);
    // nav.focus_address_bar: the active tab's breadcrumbs become a text field (path selected).
    void open_address_bar(const Workspace& ws, const Path& home, double now_ms);
    void close_address_bar() noexcept;
    [[nodiscard]] bool address_open() const noexcept { return address_.is_open(); }
    // Routes a command while the address bar has focus. True when it consumed it (list.*
    // field meanings); everything else resolves as usual.
    bool route_address(const Command& command);
    // Debounce timers and worker replies of both path fields; finished Enters land in `out`.
    // True when anything shown changed.
    bool poll_paths(double now_ms, std::vector<PathOutcome>& out);
    // A debounce or a worker job of a path field is pending (keep frames coming; scripts wait).
    [[nodiscard]] bool paths_busy() const noexcept;
    [[nodiscard]] const PathEntry& goto_entry() const noexcept { return goto_; }
    [[nodiscard]] const PathEntry& address_entry() const noexcept { return address_; }
    // Context menu at (x, y): rows run a command or apply an intent.
    struct MenuEntry {
        std::string label;
        std::string hint_command; // command name for the key hint
        std::optional<Command> command;
        std::optional<Intent> intent;
        bool enabled = true;
        bool separator_before = false;
    };
    void open_menu(const Keymap& keymap, std::vector<MenuEntry> entries, float x, float y);
    // The dragged tab while a tab drag is past its threshold (the sidebar takes drops).
    [[nodiscard]] layout::TabId dragged_tab() const noexcept;
    // A named-layout action finished: the picker drops a deleted name.
    void layout_deleted(std::string_view name);

    // ---- Drag ----------------------------------------------------------------------------------
    [[nodiscard]] bool dragging() const noexcept;
    void cancel_drag() noexcept;

    // ---- Toasts ----------------------------------------------------------------------------
    void show_toast(std::string text, bool error, double now_ms);

    // ---- Development -------------------------------------------------------------------------
    // Freeze a tab drag over `target` (screenshots), hover a tab.
    void debug_drag(layout::TabId tab, layout::PaneId from, DropTarget target);
    void debug_hover_tab(layout::TabId tab) noexcept { debug_hover_tab_ = tab; }
    // Physical rectangle of a pane's tab strip from the last update (tests, scripts).
    [[nodiscard]] std::optional<RectF> strip_rect(layout::PaneId pane) const;

private:
    enum class PopupKind : std::uint8_t {
        None,
        SearchTabs,
        SaveLayout,
        PickLayout,
        TabMenu, // right-click on a tab
        TabList, // overflow chevron of a strip
        GoTo,    // Phase 4 (nav.goto)
        Menu,    // Phase 4 generic context menu (sidebar rows)
    };
    struct MenuItem {
        std::string label;
        std::string hint_command;     // command name for the key hint; empty: none
        Command command;              // TabMenu
        layout::TabId tab;            // TabList
        std::optional<Intent> intent; // Menu: applied instead of `command` when set
        bool enabled = true;
        bool separator_before = false;
    };
    struct Popup {
        PopupKind kind = PopupKind::None;
        TextEdit field{256}; // query / name (Phase 4: a real text field)
        float field_scroll = 0.0F;
        [[nodiscard]] const std::string& query() const noexcept { return field.text(); }
        std::size_t highlight = 0;
        float scroll = 0.0F; // first visible row (rows)
        bool delete_mode = false;
        std::string confirm; // name awaiting confirmation (replace / delete)
        std::string message; // inline error or hint
        bool message_error = false;
        std::vector<TabSearchItem> items;
        std::vector<std::size_t> filtered;
        std::vector<std::string> names; // layouts
        std::vector<std::size_t> names_filtered;
        std::vector<MenuItem> menu;
        float menu_x = 0.0F;
        float menu_y = 0.0F;
        bool just_opened = true;    // the press that opened it is not an outside click
        bool pointer_moved = false; // highlight follows the pointer only after it moved
        float last_px = -1.0F;
        float last_py = -1.0F;
        WidgetId pressed = WidgetId::None;
        std::size_t pressed_index = 0;
    };
    struct TabSlot {
        layout::TabId id;
        RectF rect; // window px, scrolled
        std::string title;
        bool active = false; // active tab of its pane
    };
    struct Strip {
        layout::PaneId pane;
        RectF pane_rect;
        RectF rect;  // whole strip
        RectF tabs;  // clip area of the tabs
        RectF plus;  // "+" button
        RectF more;  // overflow chevron (w == 0: none)
        RectF theme; // theme button (w == 0: none)
        RectF body;  // the BrowserView bounds
        std::vector<TabSlot> slots;
        TabStripLayout layout;
        bool active_pane = false;
        float dim = 0.0F;
    };
    struct StripState {
        float scroll = 0.0F;
        layout::TabId shown_active;
        float dim = 0.0F; // 0 active .. 1 fully dimmed (animated)
    };
    struct SplitterDrag {
        layout::SplitId split;
        std::size_t index = 0;
        float last = -1.0F;
    };
    struct Toast {
        std::string text;
        bool error = false;
        double until_ms = 0.0;
    };

    void layout_strips(const ShellFrame& f);
    void draw_strip(const ShellFrame& f, Strip& s, const Palette& pal);
    void draw_tab(const ShellFrame& f, const Strip& s, const TabSlot& t, const Palette& pal,
                  bool hover, float alpha, bool floating);
    bool strip_button(const ShellFrame& f, const RectF& r, WidgetId id, std::size_t index,
                      const Palette& pal, bool middle_too, bool* middle_fired);
    void handle_splitters(const ShellFrame& f);
    void handle_drag(const ShellFrame& f);
    void draw_drag_overlay(const ShellFrame& f);
    void draw_toast(const ShellFrame& f);
    [[nodiscard]] PointerInput view_pointer(const ShellFrame& f) const;
    [[nodiscard]] std::vector<PaneDropArea> drop_areas() const;
    [[nodiscard]] const Strip* strip_of(layout::PaneId pane) const;

    // Popups (popups.cpp).
    void refilter();
    void move_highlight(std::ptrdiff_t delta);
    void confirm_popup(CommandQueue& queue, std::vector<Intent>& out);
    [[nodiscard]] std::size_t popup_rows() const noexcept;
    void open_tab_menu(const ShellFrame& f, layout::TabId tab, float x, float y);
    void open_tab_list(const ShellFrame& f, const Strip& s);
    void draw_popup(const ShellFrame& f);
    void draw_search_tabs(const ShellFrame& f);
    void draw_save_layout(const ShellFrame& f);
    void draw_layout_picker(const ShellFrame& f);
    void draw_menu(const ShellFrame& f);
    void draw_goto(const ShellFrame& f);
    void draw_address_dropdown(const ShellFrame& f);
    RectF draw_card(const ShellFrame& f, float width, float height, float top);
    float draw_field(const ShellFrame& f, const RectF& box, std::string_view placeholder,
                     bool search_glyph);
    void draw_footer_hints(const ShellFrame& f, const RectF& area,
                           const std::vector<std::pair<std::string, std::string>>& hints);
    float draw_keycap(const ShellFrame& f, std::string_view legend, float x, float cy,
                      bool measure_only);
    bool popup_button(const ShellFrame& f, const RectF& r, std::string_view label, bool primary,
                      bool danger, std::size_t index);

    std::map<std::uint32_t, std::unique_ptr<BrowserView>> views_;
    std::uint64_t epoch_ = 0;
    std::map<std::uint32_t, StripState> strip_state_;
    std::vector<Strip> strips_;
    PaneGeometry geom_;
    bool shell_capture_ = false; // the pointer belongs to the shell this frame
    bool over_splitter_ = false;
    PointerCursor cursor_ = PointerCursor::Default;

    WidgetId active_ = WidgetId::None; // pressed strip widget
    std::uint32_t active_id_ = 0;      // its tab or pane id
    layout::TabId middle_tab_;         // tab under a middle press
    std::uint32_t middle_plus_pane_ = 0;
    TabDrag drag_;
    float grab_dx_ = 0.0F; // pointer offset inside the dragged tab
    float grab_w_ = 0.0F;
    std::string grab_title_;
    std::optional<SplitterDrag> splitter_;
    bool debug_drag_ = false;
    layout::TabId debug_hover_tab_;

    Popup popup_;
    PathEntry goto_{PathEntryMode::GoTo};
    PathEntry address_{PathEntryMode::AddressBar};
    float address_scroll_ = 0.0F;
    RectF address_dropdown_; // last drawn dropdown card (presses inside keep the edit)
    std::optional<std::size_t> path_pressed_; // dropdown row under a press
    std::optional<Toast> toast_;
    TextFitter fitter_;
    bool fading_ = false;
    bool menu_request_ = false;
    double last_now_ms_ = 0.0;
};

} // namespace navigator::ui
