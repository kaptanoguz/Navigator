// src/ui/trash/trash_view.hpp
//
// The Trash view of a tab (Phase 7): drawn in the pane body instead of the folder list while the
// tab is in Trash mode. A top bar (back to the folder, title and count, Restore, Empty Trash),
// a Details-style list (Name, Original location, Deleted, Size; virtualized), selection by
// click / Ctrl / Shift, double-click and Enter restore, Delete asks to delete permanently
// (never the default). It reads a TrashModel and records Intents / commands; it mutates nothing.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <navigator/types.hpp>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "trash/trash_model.hpp"
#include "widgets/rect.hpp"
#include "widgets/text_fit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

struct TrashFrame {
    const Painter& painter;
    const Palette& palette;
    Theme theme;
    const TrashModel& model;
    const PointerInput& pointer;
    CommandQueue& queue;
    std::vector<Intent>& intents;
    RectF bounds; // px: the pane body below the tab strip
    std::uint32_t tab = 0;
    bool focused = true; // window focus and this is the active pane's list
    bool pane_active = true;
    bool marked = false; // accent frame (split layouts)
    bool animations = true;
    Density density = Density::Compact;
    double dt_ms = 16.0;
    Timestamp now{};
    const Path* home = nullptr;
};

class TrashView {
public:
    void draw(const TrashFrame& f);
    [[nodiscard]] std::size_t page_rows() const noexcept { return page_rows_; }
    void clear_caches() noexcept { fitter_.clear(); }

private:
    float scroll_ = 0.0F; // px
    float max_scroll_ = 0.0F;
    std::size_t page_rows_ = 10;
    std::uint64_t seen_serial_ = ~std::uint64_t{0};
    std::size_t hover_ = static_cast<std::size_t>(-1);
    WidgetId active_ = WidgetId::None;
    std::size_t active_index_ = 0;
    TextFitter fitter_;
};

} // namespace navigator::ui
