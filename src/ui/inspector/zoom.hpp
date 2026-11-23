// src/ui/inspector/zoom.hpp
//
// Phase 6 Inspector image viewport math (docs/keymap.md "Mouse (Phase 6)", docs/review/phase-5.md
// "Image inspector"): fit / 1:1 / free zoom, zoom about a point (wheel, smooth), drag panning,
// clamping, and the minimap mapping. Pure value type, no SDL.
//
// Units: "image" coordinates are pixels of the TRUE image in display orientation
// (ImageInfo::display, EXIF applied), whatever size the decoded buffer has; "view" coordinates are
// DEVICE pixels relative to the view's top-left corner. zoom = view px per image px, so 1:1 is
// zoom 1 (one image pixel per device pixel at any display scale).
//
// Rules:
//   - Fit (the default) never upscales: zoom = min(view / image, 1) per axis, the image centred.
//     Fit stays fit when the view resizes. Exception (design review Phase 6): an image whose
//     longer side is at most kTinyImagePx gets the largest INTEGER upscale that keeps it within
//     the tiny target (about 128 dp, set_tiny_target) and the view, so icons are not specks; the
//     view samples it nearest-neighbour.
//   - Free zoom ranges over [fit_zoom(), kMaxZoom] (fit for small images is 1, so they can still
//     be magnified with the wheel up to 16:1).
//   - The centre is clamped so an image larger than the view always covers it on that axis, and a
//     smaller one stays centred.
// Thread-safety: plain value; UI thread in practice.
#pragma once

#include <algorithm>
#include <cstdint>

namespace navigator::ui::inspector {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

struct Box {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
    [[nodiscard]] constexpr double right() const noexcept { return x + w; }
    [[nodiscard]] constexpr double bottom() const noexcept { return y + h; }
    friend constexpr bool operator==(const Box&, const Box&) = default;
};

enum class ZoomMode : std::uint8_t {
    Fit,      // follows the view size
    OneToOne, // zoom 1
    Free,     // any other zoom (wheel)
};

inline constexpr double kMaxZoom = 16.0;
inline constexpr double kTinyImagePx = 64.0;
// Wheel: one notch multiplies the zoom by this (smoothly animated).
inline constexpr double kWheelZoomStep = 1.25;
// The pixel grid appears from this zoom (inspector.toggle_grid).
inline constexpr double kGridMinZoom = 4.0;
// Smooth zoom time constant (ms, exponential approach).
inline constexpr double kZoomSettleMs = 70.0;

class ImageViewport {
public:
    // A new image: back to Fit. Sizes <= 0 make the viewport empty (zoom 1, nothing drawn).
    void set_image(double width, double height) noexcept;
    // The view size in device px. Fit recomputes its zoom; other modes keep zoom and re-clamp.
    void set_view(double width, double height) noexcept;
    // Device px a tiny image grows to in fit (0: never upscale). Fit re-applies.
    void set_tiny_target(double px) noexcept;

    [[nodiscard]] double image_width() const noexcept { return iw_; }
    [[nodiscard]] double image_height() const noexcept { return ih_; }
    [[nodiscard]] double view_width() const noexcept { return vw_; }
    [[nodiscard]] double view_height() const noexcept { return vh_; }
    [[nodiscard]] bool empty() const noexcept { return iw_ <= 0.0 || ih_ <= 0.0; }

    [[nodiscard]] ZoomMode mode() const noexcept { return mode_; }
    [[nodiscard]] double zoom() const noexcept { return zoom_; }
    // Zoom the animation is heading to (== zoom() when settled).
    [[nodiscard]] double target_zoom() const noexcept { return target_; }
    [[nodiscard]] double fit_zoom() const noexcept;
    [[nodiscard]] double min_zoom() const noexcept { return std::min(fit_zoom(), 1.0); }
    // Image point at the centre of the view.
    [[nodiscard]] Vec2 center() const noexcept { return center_; }

    // inspector.zoom_fit / inspector.zoom_1to1 (immediate, no animation).
    void fit() noexcept;
    // 1:1 keeping the image point under `anchor` (view px) fixed; centred when nullptr-like
    // (anchor outside the view uses the view centre).
    void one_to_one(Vec2 anchor) noexcept;
    void one_to_one() noexcept { one_to_one(Vec2{vw_ * 0.5, vh_ * 0.5}); }
    // Double-click: Fit <-> 1:1 about the pointer.
    void toggle_fit_1to1(Vec2 anchor) noexcept;

    // Immediate zoom by `factor` about `anchor` (view px), clamped to [min_zoom, kMaxZoom].
    void zoom_at(double factor, Vec2 anchor) noexcept;
    // Animated: multiplies the target by `factor` about `anchor`; call animate() every frame.
    void zoom_smooth(double factor, Vec2 anchor) noexcept;
    // Advances the smooth zoom by dt_ms; `animations` false jumps to the target. Returns true
    // while still moving.
    bool animate(double dt_ms, bool animations) noexcept;
    [[nodiscard]] bool animating() const noexcept { return zoom_ != target_; }

    // Drag: moves the image by (dx, dy) view px (the content follows the pointer).
    void pan(double dx, double dy) noexcept;
    // Keyboard panning while the Inspector has focus: one step = 1/8 of the view.
    void pan_steps(int steps_x, int steps_y) noexcept;
    // Minimap click: the image point to centre.
    void center_on(Vec2 image_point) noexcept;

    // Image larger than the view on some axis at the current zoom (drag pans, minimap shows).
    [[nodiscard]] bool pannable() const noexcept;
    // Where the whole image lands, in view px (may extend past the view).
    [[nodiscard]] Box image_rect() const noexcept;
    // The part of the image that is visible, in image px (clipped to the image).
    [[nodiscard]] Box visible_region() const noexcept;
    [[nodiscard]] Vec2 view_to_image(Vec2 v) const noexcept;
    [[nodiscard]] Vec2 image_to_view(Vec2 i) const noexcept;
    // The pixel grid is drawn (zoom >= kGridMinZoom).
    [[nodiscard]] bool grid_visible() const noexcept { return zoom_ >= kGridMinZoom - 1e-9; }

private:
    void clamp_center() noexcept;
    void apply_zoom(double z, Vec2 anchor_view, Vec2 anchor_image) noexcept;
    void update_mode() noexcept;

    double iw_ = 0.0;
    double ih_ = 0.0;
    double vw_ = 0.0;
    double vh_ = 0.0;
    double tiny_ = 0.0;
    double zoom_ = 1.0;
    double target_ = 1.0;
    Vec2 center_{};
    ZoomMode mode_ = ZoomMode::Fit;
    Vec2 anchor_view_{};
    Vec2 anchor_image_{};
};

// Minimap: the image fitted into a `max_w` x `max_h` box (aspect kept), anchored at the box's
// bottom-right corner (bx, by are the box's top-left).
[[nodiscard]] Box minimap_frame(double image_w, double image_h, double bx, double by, double max_w,
                                double max_h) noexcept;
// The visible region inside a minimap frame.
[[nodiscard]] Box minimap_visible(const ImageViewport& viewport, const Box& frame) noexcept;
// The image point under a minimap position (clamped into the image).
[[nodiscard]] Vec2 minimap_to_image(const ImageViewport& viewport, const Box& frame,
                                    Vec2 point) noexcept;

// Decode target for a fit view (device px), bucketed up to a multiple of `bucket` so small
// resizes do not request new decodes; 0 x 0 for 1:1 (full resolution).
struct DecodeTarget {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    friend constexpr bool operator==(const DecodeTarget&, const DecodeTarget&) = default;
};
[[nodiscard]] DecodeTarget bucket_target(double view_w, double view_h,
                                         std::uint32_t bucket = 256) noexcept;
// A decode of `have` still serves a fit view needing `need` (grows past it -> false).
[[nodiscard]] bool covers(DecodeTarget have, DecodeTarget need) noexcept;

} // namespace navigator::ui::inspector
