// src/ui/dnd/file_drag.hpp
//
// Drag and drop of FILES as pure logic (Phase 7, docs/architecture.md "Clipboard and drag and
// drop"; docs/dnd.md): the state machine of an internal drag (rows dragged between panes, tabs,
// breadcrumb segments, sidebar places and the Trash row), the incoming drop session fed by SDL
// drop events, the drop slots that views register while drawing, and the decision of what a
// drop does (transfer::choose_drop_action plus the filesystem facts it needs).
//
//   Idle --begin(items)--> Dragging: update() every frame picks the slot under the pointer and
//   evaluates the action from the modifiers; finish() on release returns Drop or Cancel;
//   cancel() (Escape, focus lost) returns to Idle without a change.
//
// Tab drags (Phase 3, drag_drop.hpp) are a different machine; both never run together.
// The view records the press, this machine owns everything after the threshold. No SDL, no
// renderer; filesystem facts come through DropProbe so tests use fixtures or fakes.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/transfer.hpp>
#include <navigator/types.hpp>

#include "widgets/rect.hpp"

namespace navigator::ui {

enum class SlotKind : std::uint8_t {
    Pane,    // a pane's list area: the folder it shows
    Tab,     // a tab in a strip: that tab's folder
    Crumb,   // a breadcrumb segment
    Row,     // a folder row of a list
    Sidebar, // a place / bookmark / volume row
    Trash,   // the sidebar Trash row: file.trash for the dragged items
};

// Where a drop can land, registered by the views every frame (physical px).
struct DropSlot {
    RectF rect;
    Path folder; // destination folder (empty for Trash)
    SlotKind kind = SlotKind::Pane;
    std::uint32_t tab = 0; // the tab the slot belongs to (Pane / Tab / Crumb / Row)
    std::string label;     // "Documents": shown in the badge
};

// Slot under (x, y): the most specific kind wins (Row > Trash > Tab / Crumb / Sidebar > Pane),
// later registrations win ties. nullptr when none.
[[nodiscard]] const DropSlot* pick_slot(const std::vector<DropSlot>& slots, float x, float y);

// Filesystem facts for the decision. The default implementation uses lstat / access.
class DropProbe {
public:
    DropProbe() = default;
    virtual ~DropProbe() = default;
    DropProbe(const DropProbe&) = delete;
    DropProbe& operator=(const DropProbe&) = delete;
    DropProbe(DropProbe&&) = delete;
    DropProbe& operator=(DropProbe&&) = delete;
    [[nodiscard]] virtual std::optional<std::uint64_t> device(const Path& path) = 0; // st_dev
    [[nodiscard]] virtual bool writable(const Path& folder) = 0;                     // access W_OK
};
[[nodiscard]] DropProbe& default_drop_probe();

struct DropEval {
    transfer::DropAction action = transfer::DropAction::None;
    Path folder;        // the destination folder (empty for Trash)
    bool trash = false; // the Trash row: move the items to the trash
    SlotKind kind = SlotKind::Pane;
    std::string label; // slot label
    RectF rect;        // where to draw the highlight
    [[nodiscard]] bool allowed() const noexcept {
        return trash || action != transfer::DropAction::None;
    }
};

// What dropping `sources` on `slot` would do with `mods` held. internal: the drag started in
// this process (Move within a filesystem); external drops copy unless Shift is held.
[[nodiscard]] DropEval evaluate_drop(const std::vector<Path>& sources, const DropSlot* slot,
                                     bool internal, Modifiers mods, DropProbe& probe);

// Spring-loaded folders: a folder row hovered this long opens (disabled without animations).
inline constexpr double kSpringLoadMs = 700.0;
// Pointer travel (dp) before a press on a row becomes a drag (the view applies it).
inline constexpr float kFileDragThresholdDp = 4.0F;

class FileDrag {
public:
    struct Outcome {
        enum class Kind : std::uint8_t { Cancel, Drop };
        Kind kind = Kind::Cancel;
        std::vector<Path> items;
        DropEval eval;
    };

    // Starts dragging `items` that were selected in `origin_tab`.
    void begin(std::vector<Path> items, std::uint32_t origin_tab, double now_ms);
    // Every frame while dragging: hover and evaluate. `slots` are the slots registered during
    // the PREVIOUS frame's draw (hit testing needs the geometry of what was on screen).
    void update(float x, float y, Modifiers mods, const std::vector<DropSlot>& slots, double now_ms,
                DropProbe& probe);
    // Button released: Drop when the hovered slot allows it, else Cancel. Returns to Idle.
    Outcome finish();
    void cancel() noexcept;

    [[nodiscard]] bool dragging() const noexcept { return active_; }
    [[nodiscard]] const std::vector<Path>& items() const noexcept { return items_; }
    [[nodiscard]] std::uint32_t origin_tab() const noexcept { return origin_tab_; }
    [[nodiscard]] const DropEval& eval() const noexcept { return eval_; }
    [[nodiscard]] const std::optional<DropSlot>& hover() const noexcept { return hover_; }
    [[nodiscard]] float x() const noexcept { return x_; }
    [[nodiscard]] float y() const noexcept { return y_; }
    [[nodiscard]] Modifiers mods() const noexcept { return mods_; }

    // The folder row (or sidebar / tab slot) hovered for kSpringLoadMs: its tab and folder, once
    // per hover. nullopt otherwise.
    [[nodiscard]] std::optional<DropSlot> take_spring(double now_ms);

private:
    bool active_ = false;
    std::vector<Path> items_;
    std::uint32_t origin_tab_ = 0;
    std::optional<DropSlot> hover_;
    DropEval eval_;
    float x_ = 0.0F;
    float y_ = 0.0F;
    Modifiers mods_ = Modifiers::None;
    double hover_since_ms_ = 0.0;
    bool spring_fired_ = false;
};

// A drop session from another program (SDL_EVENT_DROP_BEGIN .. DROP_COMPLETE).
class IncomingDrop {
public:
    void begin();
    void position(float x, float y);
    void add_path(const Path& path);
    // DROP_TEXT: decode_uri_list(); returns how many local paths it added.
    std::size_t add_text(std::string_view text);
    // DROP_COMPLETE: the collected paths (the session ends).
    [[nodiscard]] std::vector<Path> complete();
    void cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const std::vector<Path>& paths() const noexcept { return paths_; }
    [[nodiscard]] float x() const noexcept { return x_; }
    [[nodiscard]] float y() const noexcept { return y_; }
    [[nodiscard]] bool has_position() const noexcept { return positioned_; }

private:
    bool active_ = false;
    bool positioned_ = false;
    std::vector<Path> paths_;
    float x_ = 0.0F;
    float y_ = 0.0F;
};

} // namespace navigator::ui
