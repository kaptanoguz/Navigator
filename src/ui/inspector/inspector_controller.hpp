// src/ui/inspector/inspector_controller.hpp
//
// Phase 6 Inspector program logic, headless (docs/architecture.md "Phase 6", docs/keymap.md
// "Phase 6 defaults"): which item the Inspector shows (the active pane's cursor, followed with a
// short debounce so holding an arrow key does not queue a decode per row; or the locked item),
// the preview request for it (fit target bucketed by the view size, full resolution at 1:1),
// tickets and completions, refresh in place, the multi-selection summary, and the pure dock
// geometry (placement, size clamps, list minimum). The view (inspector_view) draws what this
// says; the App owns one controller and feeds it at frame start and frame end.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/layout.hpp>
#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/types.hpp>

#include "widgets/rect.hpp"

namespace navigator::ui::inspector {

// Cursor changes settle this long before a decode is submitted (a memory-cache hit shows at once).
inline constexpr double kFollowDebounceMs = 90.0;
// Neighbours of the inspected image prefetched (cursor +-1 .. +-kPrefetchNeighbours).
inline constexpr std::size_t kPrefetchNeighbours = 2;

// Something the Inspector can show: a listing entry and its absolute path.
struct Item {
    Path path;
    fs::DirEntry entry;
    preview::SourceLocation location;
    preview::FolderOptions folder; // the inspecting tab's view options (folders)

    // Same file version (path, mtime, size): no new request.
    [[nodiscard]] bool same_version(const Item& o) const noexcept {
        return path == o.path && entry.mtime == o.entry.mtime && entry.size == o.entry.size &&
               entry.kind == o.entry.kind;
    }
};

// What the active pane offers this frame.
struct Subject {
    std::optional<Item> cursor;     // cursor entry (nullopt: empty folder / no cursor)
    std::size_t selected_count = 0; // > 1: multi-selection summary instead of a preview
    std::uint64_t selected_bytes = 0;
    std::vector<Item> neighbours; // prefetch candidates (images near the cursor)
};

enum class Status : std::uint8_t {
    Empty,   // nothing to show (no cursor)
    Multi,   // multi-selection summary
    Loading, // request outstanding, nothing (or a stale result) to show
    Ready,   // result() is the item's preview
    Failed,  // error() says why
};

// Visual content (images, video posters) depends on the view size; the rest does not.
[[nodiscard]] bool is_visual_mime(std::string_view mime) noexcept;

class Controller {
public:
    explicit Controller(preview::IPreviewService* service) noexcept : service_(service) {}
    void set_service(preview::IPreviewService* service) noexcept { service_ = service; }

    // ---- Lock ------------------------------------------------------------------------------
    [[nodiscard]] bool locked() const noexcept { return locked_; }
    // inspector.toggle_lock: locks the current item (no-op without one), or unlocks.
    void toggle_lock() noexcept;
    // The locked item vanished (deleted / renamed away): unlocks. Returns true when it did.
    bool unlock_if_gone(bool exists) noexcept;
    // Phase 7: the shown item was moved or renamed from `from`: it now is `moved` (same lock,
    // refreshed in place). False when the Inspector shows something else.
    bool retarget(const Path& from, Item moved, double now_ms);

    // ---- Frame start / end -------------------------------------------------------------------
    // Shown or hidden (view.inspect). Hiding cancels the outstanding request.
    void set_visible(bool visible, double now_ms);
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    // The active pane's subject; ignored while locked (except the summary, which never shows
    // while locked: the locked item stays).
    void follow(const Subject& subject, double now_ms);
    // Fit target of the view in device px (bucketed by the caller with bucket_target), or
    // full resolution for 1:1. Visual items only; a larger target than the decoded one requests
    // a new decode, the shown result stays until it lands.
    void set_view_target(preview::PixelSize fit_target, bool full_resolution, double now_ms);
    // Submits a debounced request when due; prefetches neighbours of a shown image. `epoch` is
    // the ui's thumbnail epoch (prefetch jobs are dropped with off-screen thumbnails).
    void tick(double now_ms, std::uint64_t epoch);
    // A polled completion; true when it was one of ours.
    bool on_completion(const preview::Completion& completion);
    // Watch event / F5 on the item: drops its cached state so it is requested again.
    void invalidate() noexcept;

    // ---- What to draw ------------------------------------------------------------------------
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] const Item* item() const noexcept { return item_ ? &*item_ : nullptr; }
    // The finished preview of the item (or, while Loading after a refresh / resize, the previous
    // result of the same item: refresh in place, no flash).
    [[nodiscard]] const std::shared_ptr<const preview::PreviewResult>& result() const noexcept {
        return result_;
    }
    [[nodiscard]] bool result_is_current() const noexcept { return result_current_; }
    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t selected_count() const noexcept { return selected_count_; }
    [[nodiscard]] std::uint64_t selected_bytes() const noexcept { return selected_bytes_; }
    // Key of the current request (the view keys its texture on result keys).
    [[nodiscard]] const std::optional<preview::PreviewKey>& result_key() const noexcept {
        return result_key_;
    }
    // Changes whenever item / result / status changes (the view restarts its fade).
    [[nodiscard]] std::uint64_t serial() const noexcept { return serial_; }
    // Debounce or a request is outstanding (keep frames coming to deliver it).
    [[nodiscard]] bool busy() const noexcept { return due_ms_ >= 0.0 || ticket_.valid(); }
    // Submissions so far (tests).
    [[nodiscard]] std::uint64_t submitted() const noexcept { return submitted_; }

private:
    [[nodiscard]] preview::PreviewRequest build_request(const Item& item) const;
    void adopt(Item item, double now_ms, bool refresh);
    void request(double now_ms, bool immediate);
    void submit_now();
    void cancel() noexcept;

    preview::IPreviewService* service_ = nullptr;
    bool visible_ = false;
    bool locked_ = false;
    std::optional<Item> item_;
    std::size_t selected_count_ = 0;
    std::uint64_t selected_bytes_ = 0;
    preview::PixelSize fit_target_{1024, 1024};
    bool full_resolution_ = false;
    std::optional<preview::PreviewKey> want_key_;   // key the current item / target needs
    std::optional<preview::PreviewKey> result_key_; // key of result_
    std::shared_ptr<const preview::PreviewResult> result_;
    bool result_current_ = false;
    std::optional<Error> error_;
    preview::Ticket ticket_;
    double due_ms_ = -1.0; // debounce deadline, -1: none
    double last_change_ms_ = -1.0e9;
    std::uint64_t serial_ = 1;
    std::uint64_t submitted_ = 0;
    std::vector<Item> neighbours_;
    std::vector<preview::Ticket> prefetch_;
    bool prefetched_ = false;
};

// ---- Keys local to a focused Inspector (pure) ------------------------------------------------
// While the Inspector has the keyboard (after a click), these plain keys act on the image; they
// never fire while the list has focus and have no keymap rows (Navigator's choice, design review
// Phase 6 optional 2): 0 fit, 1 1:1, + / = zoom in, - zoom out, G grid, M minimap.
enum class LocalKey : std::uint8_t { Fit, OneToOne, ZoomIn, ZoomOut, Grid, Minimap };
[[nodiscard]] std::optional<LocalKey> inspector_local_key(const KeyChord& chord) noexcept;

// ---- Dock geometry (pure) -------------------------------------------------------------------

struct DockLayout {
    layout::InspectorPlacement placement = layout::InspectorPlacement::Right; // resolved
    RectF list;      // the pane tree's area
    RectF inspector; // the Inspector card's area (empty when hidden)
    RectF splitter;  // the resize handle on the Inspector's inner edge (px)
};

// `content`: the area right of the sidebar (px); `scale`: px per dp; `window_w/h_dp`: the logical
// window size for Auto. Clamps the extent to [kInspectorMin*, kInspectorMax*] dp and keeps the
// list at least kInspectorMinListExtent dp, shrinking the Inspector first (below its minimum when
// the window is that small, but never below 120 dp).
[[nodiscard]] DockLayout dock_inspector(const layout::InspectorState& state, const RectF& content,
                                        float scale, double window_w_dp, double window_h_dp);

// Splitter drag: the new extent (dp) for a pointer at `pointer_px`, clamped like dock_inspector.
[[nodiscard]] double drag_extent(layout::InspectorPlacement placement, const RectF& content,
                                 float scale, float pointer_px) noexcept;

// InspectorState with width / height clamped to their ranges.
[[nodiscard]] layout::InspectorState clamped(layout::InspectorState state) noexcept;

} // namespace navigator::ui::inspector
