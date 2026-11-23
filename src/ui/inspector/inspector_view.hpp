// src/ui/inspector/inspector_view.hpp
//
// Phase 6 Inspector pane, immediate mode (docs/review/phase-5.md "Notes for Phase 6",
// docs/keymap.md "Phase 6 defaults" / "Mouse (Phase 6)"). Draws inside the dock rect from
// dock_inspector(): a surface_raised card with a 1dp border (the focus ring while it has the
// keyboard), a header (name, type and size, lock / placement buttons, "Play" for videos), and
// the content of the controller's status:
//   image   fit / 1:1 / wheel zoom about the pointer, drag to pan, double-click toggles fit / 1:1,
//           a floating zoom toolbar, minimap while zoomed, pixel grid at >= 4:1, neutral
//           checkerboard under transparency, skeleton (with the list thumbnail) while decoding,
//           fade-in unless animations are off
//   video   poster (or the type glyph) with a centred play disc, metadata, "details unavailable"
//   text    monospace, line numbers, soft wrap of the preview's 1 KiB rows, virtualized
//   folder  summary and the first entries, virtualized (never navigates)
//   states  empty, multi-selection summary, loading, unsupported / coming soon / binary, error
// followed by the grouped key/value metadata (click a value to copy it).
// The view never mutates program state: it enqueues commands and reports InspectorOutput, which
// the App applies at frame end.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/layout.hpp>
#include <navigator/preview.hpp>
#include <navigator/types.hpp>

#include "inspector/inspector_controller.hpp"
#include "inspector/metadata.hpp"
#include "inspector/text_layout.hpp"
#include "inspector/texture_cache.hpp"
#include "inspector/zoom.hpp"
#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/text_fit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui::inspector {

struct InspectorFrame {
    const Painter& painter;
    const Palette& palette;
    Theme theme;
    const PointerInput& pointer; // masked by the App while a popup is open
    CommandQueue& queue;
    const Controller& controller;
    const layout::InspectorState& state;
    TextureCache& textures;
    RectF dock;     // px: the Inspector's area (card drawn inside with margins)
    RectF content;  // px: the area shared with the panes (splitter drag clamps against it)
    RectF splitter; // px: resize handle
    layout::InspectorPlacement placement = layout::InspectorPlacement::Right;
    bool keyboard = false; // the Inspector has keyboard focus
    bool window_focused = true;
    bool animations = true;
    Density density = Density::Compact;
    double now_ms = 0.0;
    double dt_ms = 16.0;
    Timestamp now{};
    // The item's list thumbnail (low-resolution placeholder while the image decodes), or null.
    const TextureCache::Texture* placeholder = nullptr;
};

// Applied by the App at frame end.
struct InspectorOutput {
    std::optional<double> resize;    // new width (Right) / height (Bottom) in dp
    bool pressed_inside = false;     // a press in the card: keyboard focus moves to the Inspector
    std::optional<std::string> copy; // a metadata value to put on the clipboard
};

class InspectorView {
public:
    void draw(const InspectorFrame& f, InspectorOutput& out);

    // Commands routed by the App (docs/keymap.md "Focus scoping (Phase 6)").
    void zoom_fit() noexcept;
    void zoom_1to1() noexcept;
    // list.cursor_* while the Inspector has focus: pan an image, scroll text / folder / metadata.
    void cursor(CommandId id) noexcept;

    // The fit decode target (device px, bucketed) and whether the zoom needs full resolution;
    // read by the App after draw() and handed to the controller.
    [[nodiscard]] preview::PixelSize fit_target() const noexcept { return fit_target_; }
    [[nodiscard]] bool wants_full_resolution() const noexcept { return full_res_; }
    // Animating (zoom, fade, skeleton shimmer, a drag): keep frames coming.
    [[nodiscard]] bool animating() const noexcept { return animating_; }
    // The splitter is hovered or dragged (resize cursor).
    [[nodiscard]] bool resize_hot() const noexcept { return resize_hot_ || resizing_; }
    [[nodiscard]] const RectF& card() const noexcept { return card_; }
    [[nodiscard]] const ImageViewport& viewport() const noexcept { return viewport_; }
    void clear_caches() noexcept { fitter_.clear(); }

private:
    enum class Hot : std::uint8_t {
        None,
        Splitter,
        Image,
        Minimap,
        Play,
        Lock,
        Placement,
        Fit,
        OneToOne,
        Grid,
        Minimap2, // toolbar minimap toggle
        Text,
        Meta,
        MetaRow,
        Scroll,
    };

    void handle_splitter(const InspectorFrame& f, InspectorOutput& out);
    void draw_header(const InspectorFrame& f, const RectF& area);
    bool icon_button(const InspectorFrame& f, const RectF& r, Hot id, int glyph, bool on,
                     bool enabled = true);
    void draw_body(const InspectorFrame& f, const RectF& area, InspectorOutput& out);
    void draw_media(const InspectorFrame& f, const RectF& area, const preview::PreviewResult* res);
    void draw_image(const InspectorFrame& f, const RectF& view, const TextureCache::Texture* tex,
                    const preview::ImageInfo* info);
    void draw_toolbar(const InspectorFrame& f, const RectF& view);
    void draw_minimap(const InspectorFrame& f, const RectF& view, const TextureCache::Texture& tex);
    void draw_skeleton(const InspectorFrame& f, const RectF& area, double aspect);
    void draw_play_disc(const InspectorFrame& f, const RectF& area);
    void draw_text(const InspectorFrame& f, const RectF& area, const preview::TextPreview& text);
    void draw_folder(const InspectorFrame& f, const RectF& area,
                     const preview::FolderPreview& folder);
    void draw_state(const InspectorFrame& f, const RectF& area, std::string_view title,
                    std::string_view detail, int glyph);
    void draw_meta(const InspectorFrame& f, const RectF& area, InspectorOutput& out);
    void draw_scrollbar(const InspectorFrame& f, const RectF& track, double offset, double max,
                        double view);

    // Pointer capture.
    Hot active_ = Hot::None;
    float last_x_ = 0.0F;
    float last_y_ = 0.0F;
    bool resize_hot_ = false;
    bool resizing_ = false;
    bool animating_ = false;

    // Content state.
    RectF card_;
    RectF media_;
    std::uint64_t seen_serial_ = 0;
    Path shown_path_;
    ImageViewport viewport_;
    std::optional<preview::PreviewKey> shown_key_; // texture on screen
    double fade_start_ms_ = -1.0;
    preview::PixelSize fit_target_{1024, 1024};
    bool full_res_ = false;
    std::size_t decoded_width_ = 0;
    // Text / folder / metadata scrolling (px).
    double text_scroll_ = 0.0;
    double text_max_ = 0.0;
    double text_page_ = 0.0;
    double meta_scroll_ = 0.0;
    double meta_max_ = 0.0;
    TextWrap wrap_;
    std::vector<MetaGroup> meta_;
    std::uint64_t meta_serial_ = 0;
    std::string meta_language_;
    TextFitter fitter_;
    std::string tooltip_;
};

} // namespace navigator::ui::inspector
