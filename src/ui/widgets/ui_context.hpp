// src/ui/widgets/ui_context.hpp
//
// Per-frame input snapshot and output channels for immediate-mode widgets.
//   - PointerInput: mouse state in physical pixels, gathered from SDL events by the App before
//     the frame is built (press/release/click flags last one frame).
//   - Intent: a pointer gesture that carries a row, a rectangle or a path, recorded while
//     drawing and applied by the App at frame end BEFORE the command queue is drained
//     (docs/architecture.md). Gestures with a command equivalent enqueue the command instead.
// Widgets never mutate program state: they read the model, write intents / commands, and keep
// only their own view state (hover, pressed widget, scroll animation).
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/layout.hpp>
#include <navigator/types.hpp>

#include "workspace/drag_drop.hpp"

namespace navigator::ui {

struct PointerInput {
    float x = -1.0F; // px, window coordinates
    float y = -1.0F;
    bool inside = false;              // pointer is over the window
    bool down = false;                // left button held
    bool pressed = false;             // left button went down this frame
    bool released = false;            // left button went up this frame
    int clicks = 0;                   // SDL click count of this frame's press (2 = double-click)
    Modifiers mods = Modifiers::None; // modifiers held at the last button/key event
    float wheel_y = 0.0F;             // wheel notches this frame (positive = away from the user)
    float wheel_x = 0.0F;             // horizontal wheel notches (positive = right)
    // Phase 3: middle and right buttons (tab strip, middle-click on rows, context menus).
    bool middle_pressed = false;
    bool middle_released = false;
    bool right_pressed = false;
};

struct Intent {
    enum class Kind : std::uint8_t {
        ClickRow,   // row, mods
        BandBegin,  // additive
        BandUpdate, // row..row_last (row > row_last: none)
        BandEnd,
        ClearSelection, // click on empty space
        SortBy,         // sort_key
        NavigateTo,     // path (breadcrumb segment)
        DismissNotice,
        // Phase 3 (workspace level unless noted).
        CursorTo,      // row (tab's model): middle-click, selection unchanged
        ActivatePane,  // pane
        ActivateTab,   // tab
        DropTab,       // tab, drop
        MoveSplitter,  // split, index, position, min_fraction
        EqualizeSplit, // split
        ReopenClosed,  // index into ClosedTabs::entries()
        SaveLayout,    // text (name)
        LoadLayout,    // text
        DeleteLayout,  // text
    };
    Kind kind = Kind::ClickRow;
    // The tab whose model a list intent applies to (layout::TabId value); set by the view.
    std::uint32_t tab = 0;
    layout::PaneId pane;
    layout::SplitId split;
    std::size_t index = 0;
    double position = 0.0;
    double min_fraction = 0.0;
    DropTarget drop;
    std::string text;
    std::size_t row = 0;
    std::size_t row_last = 0;
    Modifiers mods = Modifiers::None;
    bool additive = false;
    fs::SortKey sort_key = fs::SortKey::Name;
    Path path;
};

// Identifies the widget that owns a press until release ("active" in IMGUI terms).
enum class WidgetId : std::uint16_t {
    None,
    Back,
    Forward,
    Up,
    Theme,
    Crumb, // + crumb index in UiState::active_index
    Header,
    Rows,
    Band,
    Scrollbar,
    DismissNotice,
    // Phase 3 (workspace view).
    Tab,
    TabClose,
    TabPlus,
    TabMore,
    Splitter,
    PopupRow,
    PopupButton,
    PopupDelete,
};

} // namespace navigator::ui
