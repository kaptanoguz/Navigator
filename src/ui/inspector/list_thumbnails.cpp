// src/ui/inspector/list_thumbnails.cpp — list row thumbnails. See list_thumbnails.hpp.
#include "inspector/list_thumbnails.hpp"

#include <algorithm>
#include <cmath>

#include <navigator/mime.hpp>

namespace navigator::ui::inspector {

namespace {

preview::PixelSize box_target(float box_px) {
    const auto side = static_cast<std::uint32_t>(std::max(1.0F, std::ceil(box_px)));
    return preview::PixelSize{side, side};
}

} // namespace

bool ListThumbnails::draw_row(Renderer& r, const Palette& pal, const BrowserModel& model,
                              std::size_t row, const RectF& box, float scale, float dim) {
    if (!scheduler_->enabled()) {
        return false;
    }
    const fs::DirEntry& e = model.row_entry(row);
    const std::string_view mime = mime::guess_for_entry(e);
    if (!wants_thumbnail(mime, e)) {
        return false;
    }
    const Path path = model.row_path(row);
    preview::PreviewRequest req =
        make_request(path, e, preview::PreviewPurpose::Thumbnail, box_target(box.w));
    if (location_) {
        req.location = location_(path);
    }
    const preview::PreviewKey key = preview_key(req);
    const std::shared_ptr<const preview::PreviewResult> result =
        scheduler_->want(key, req, preview::RequestPriority::Visible);
    const TextureCache::Texture* tex = textures_->get(key);
    if (tex == nullptr) {
        if (result) {
            textures_->request(key, result, UploadClass::Thumbnail);
        }
        return false;
    }
    // Contain: the whole thumbnail, aspect kept, centred in the box, whole pixels.
    const auto tw = static_cast<float>(tex->width);
    const auto th = static_cast<float>(tex->height);
    const float s = std::min(box.w / tw, box.h / th);
    const float w = std::max(2.0F, std::round(tw * s));
    const float h = std::max(2.0F, std::round(th * s));
    const RectF dst{std::round(box.center_x() - (0.5F * w)),
                    std::round(box.center_y() - (0.5F * h)), w, h};
    float alpha = dim;
    if (animations_ && tex->ready_ms >= 0.0) {
        const double t = (now_ms_ - tex->ready_ms) / kThumbnailFadeMs;
        if (t < 1.0) {
            fading_ = true;
            alpha *= static_cast<float>(std::clamp(t, 0.0, 1.0));
        }
    }
    const float rad = std::round(3.0F * scale);
    const float hair = std::max(1.0F, std::round(scale));
    // A quiet plate behind transparent thumbnails keeps the frame readable on every row fill.
    r.fill_rounded_rect(dst, rad, pal.icon_page.with_alpha(pal.icon_page.a * alpha));
    r.draw_image_rounded(tex->tiles.empty() ? nullptr : tex->tiles.front().texture, dst, rad,
                         alpha);
    r.stroke_rounded_rect(dst, rad, hair, pal.icon_page_border.with_alpha(0.9F * alpha));
    return true;
}

void ListThumbnails::prefetch_row(const BrowserModel& model, std::size_t row, float box_px) {
    if (!scheduler_->enabled() || row >= model.row_count()) {
        return;
    }
    const fs::DirEntry& e = model.row_entry(row);
    const std::string_view mime = mime::guess_for_entry(e);
    if (!wants_thumbnail(mime, e)) {
        return;
    }
    const Path path = model.row_path(row);
    preview::PreviewRequest req =
        make_request(path, e, preview::PreviewPurpose::Thumbnail, box_target(box_px));
    if (location_) {
        req.location = location_(path);
    }
    const preview::PreviewKey key = preview_key(req);
    if (const auto result = scheduler_->want(key, req, preview::RequestPriority::Prefetch);
        result) {
        textures_->request(key, result, UploadClass::Thumbnail);
    }
}

} // namespace navigator::ui::inspector
