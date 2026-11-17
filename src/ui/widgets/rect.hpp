// src/ui/widgets/rect.hpp
//
// Axis-aligned rectangle in physical pixels, shared by the renderer and the headless layout math
// (pane geometry, tab strips, drop zones). No SDL here so tests and navigator_ui_browser can use
// it without a window.
// Thread-safety: plain value type and pure functions.
#pragma once

namespace navigator::ui {

struct RectF {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;

    [[nodiscard]] constexpr float right() const noexcept { return x + w; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + h; }
    [[nodiscard]] constexpr float center_x() const noexcept { return x + (0.5F * w); }
    [[nodiscard]] constexpr float center_y() const noexcept { return y + (0.5F * h); }

    friend constexpr bool operator==(const RectF&, const RectF&) = default;
};

[[nodiscard]] constexpr bool contains(const RectF& r, float px, float py) noexcept {
    return px >= r.x && px < r.right() && py >= r.y && py < r.bottom();
}

} // namespace navigator::ui
