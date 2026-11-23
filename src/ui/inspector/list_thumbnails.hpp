// src/ui/inspector/list_thumbnails.hpp
//
// Phase 6: the list's RowThumbnailSource. For an image / video row it builds the Thumbnail request
// (device-pixel box, volume location for the generation policy), asks the ThumbnailScheduler,
// queues the upload of a finished result in the TextureCache, and draws a landed texture:
// aspect-correct ("contain") in the glyph box, rounded 3dp corners, the 1px icon_page_border frame
// (design review), faded in over 160 ms unless animations are off. Until then the type glyph stays
// and the row never changes height.
// Thread-safety: UI thread only.
#pragma once

#include <functional>

#include <navigator/preview.hpp>

#include "browser/browser_view.hpp"
#include "inspector/texture_cache.hpp"
#include "inspector/thumbnail_scheduler.hpp"

namespace navigator::ui::inspector {

inline constexpr double kThumbnailFadeMs = 160.0;

class ListThumbnails final : public RowThumbnailSource {
public:
    using LocationFn = std::function<preview::SourceLocation(const Path& path)>;

    ListThumbnails(ThumbnailScheduler& scheduler, TextureCache& textures, LocationFn location)
        : scheduler_(&scheduler), textures_(&textures), location_(std::move(location)) {}

    // Per frame, before the views draw.
    void begin_frame(double now_ms, bool animations) noexcept {
        now_ms_ = now_ms;
        animations_ = animations;
        fading_ = false;
    }
    // A thumbnail is fading in (keep frames coming).
    [[nodiscard]] bool fading() const noexcept { return fading_; }

    bool draw_row(Renderer& r, const Palette& pal, const BrowserModel& model, std::size_t row,
                  const RectF& box, float scale, float dim) override;
    void prefetch_row(const BrowserModel& model, std::size_t row, float box_px) override;

private:
    ThumbnailScheduler* scheduler_;
    TextureCache* textures_;
    LocationFn location_;
    double now_ms_ = 0.0;
    bool animations_ = true;
    bool fading_ = false;
};

} // namespace navigator::ui::inspector
