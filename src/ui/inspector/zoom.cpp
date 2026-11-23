// src/ui/inspector/zoom.cpp — Inspector image viewport math. See zoom.hpp.
#include "inspector/zoom.hpp"

#include <algorithm>
#include <cmath>

namespace navigator::ui::inspector {

void ImageViewport::set_image(double width, double height) noexcept {
    iw_ = width > 0.0 ? width : 0.0;
    ih_ = height > 0.0 ? height : 0.0;
    fit();
}

void ImageViewport::set_view(double width, double height) noexcept {
    vw_ = std::max(0.0, width);
    vh_ = std::max(0.0, height);
    if (mode_ == ZoomMode::Fit && !animating()) {
        fit();
        return;
    }
    const double lo = min_zoom();
    zoom_ = std::clamp(zoom_, lo, kMaxZoom);
    target_ = std::clamp(target_, lo, kMaxZoom);
    clamp_center();
    update_mode();
}

double ImageViewport::fit_zoom() const noexcept {
    if (empty() || vw_ <= 0.0 || vh_ <= 0.0) {
        return 1.0;
    }
    return std::min({vw_ / iw_, vh_ / ih_, 1.0});
}

void ImageViewport::fit() noexcept {
    zoom_ = fit_zoom();
    target_ = zoom_;
    center_ = Vec2{iw_ * 0.5, ih_ * 0.5};
    mode_ = ZoomMode::Fit;
}

void ImageViewport::one_to_one(Vec2 anchor) noexcept {
    if (anchor.x < 0.0 || anchor.y < 0.0 || anchor.x > vw_ || anchor.y > vh_) {
        anchor = Vec2{vw_ * 0.5, vh_ * 0.5};
    }
    apply_zoom(1.0, anchor, view_to_image(anchor));
    target_ = zoom_;
    mode_ = ZoomMode::OneToOne;
}

void ImageViewport::toggle_fit_1to1(Vec2 anchor) noexcept {
    if (mode_ == ZoomMode::Fit && std::abs(fit_zoom() - 1.0) > 1e-9) {
        one_to_one(anchor);
    } else {
        fit();
    }
}

void ImageViewport::apply_zoom(double z, Vec2 anchor_view, Vec2 anchor_image) noexcept {
    zoom_ = std::clamp(z, min_zoom(), kMaxZoom);
    // Keep anchor_image under anchor_view.
    center_.x = anchor_image.x - ((anchor_view.x - (vw_ * 0.5)) / zoom_);
    center_.y = anchor_image.y - ((anchor_view.y - (vh_ * 0.5)) / zoom_);
    clamp_center();
}

void ImageViewport::update_mode() noexcept {
    if (animating()) {
        return;
    }
    if (std::abs(zoom_ - fit_zoom()) < 1e-9 && !pannable()) {
        mode_ = ZoomMode::Fit;
    } else if (std::abs(zoom_ - 1.0) < 1e-9) {
        mode_ = ZoomMode::OneToOne;
    } else {
        mode_ = ZoomMode::Free;
    }
}

void ImageViewport::zoom_at(double factor, Vec2 anchor) noexcept {
    if (empty() || factor <= 0.0) {
        return;
    }
    apply_zoom(zoom_ * factor, anchor, view_to_image(anchor));
    target_ = zoom_;
    update_mode();
}

void ImageViewport::zoom_smooth(double factor, Vec2 anchor) noexcept {
    if (empty() || factor <= 0.0) {
        return;
    }
    const double next = std::clamp(target_ * factor, min_zoom(), kMaxZoom);
    // Snap to 1:1 when the wheel crosses it, so "one image pixel per device pixel" is reachable.
    const bool crosses_one = (target_ < 1.0 && next > 1.0) || (target_ > 1.0 && next < 1.0);
    target_ = crosses_one ? 1.0 : next;
    anchor_view_ = anchor;
    anchor_image_ = view_to_image(anchor);
    if (target_ != zoom_) {
        mode_ = ZoomMode::Free;
    }
}

bool ImageViewport::animate(double dt_ms, bool animations) noexcept {
    if (!animating()) {
        return false;
    }
    double z = target_;
    if (animations && dt_ms > 0.0) {
        const double k = std::exp(-dt_ms / kZoomSettleMs);
        // Interpolate in log space: the motion looks uniform at every magnification.
        z = std::exp(std::log(target_) + ((std::log(zoom_) - std::log(target_)) * k));
        if (std::abs(z - target_) / target_ < 0.002) {
            z = target_;
        }
    }
    apply_zoom(z, anchor_view_, anchor_image_);
    if (std::abs(zoom_ - target_) < 1e-12 || z == target_) {
        zoom_ = target_;
        apply_zoom(zoom_, anchor_view_, anchor_image_);
    }
    update_mode();
    return animating();
}

void ImageViewport::pan(double dx, double dy) noexcept {
    if (empty()) {
        return;
    }
    center_.x -= dx / zoom_;
    center_.y -= dy / zoom_;
    clamp_center();
}

void ImageViewport::pan_steps(int steps_x, int steps_y) noexcept {
    pan(-static_cast<double>(steps_x) * vw_ / 8.0, -static_cast<double>(steps_y) * vh_ / 8.0);
}

void ImageViewport::center_on(Vec2 image_point) noexcept {
    center_ = image_point;
    clamp_center();
}

bool ImageViewport::pannable() const noexcept {
    if (empty()) {
        return false;
    }
    return (iw_ * zoom_) > vw_ + 0.5 || (ih_ * zoom_) > vh_ + 0.5;
}

void ImageViewport::clamp_center() noexcept {
    const auto axis = [this](double& c, double image, double view) {
        const double half = view / (2.0 * zoom_);
        if (image <= 2.0 * half) {
            c = image * 0.5;
        } else {
            c = std::clamp(c, half, image - half);
        }
    };
    axis(center_.x, iw_, vw_);
    axis(center_.y, ih_, vh_);
}

Box ImageViewport::image_rect() const noexcept {
    const Vec2 tl = image_to_view(Vec2{0.0, 0.0});
    return Box{tl.x, tl.y, iw_ * zoom_, ih_ * zoom_};
}

Box ImageViewport::visible_region() const noexcept {
    const Vec2 a = view_to_image(Vec2{0.0, 0.0});
    const Vec2 b = view_to_image(Vec2{vw_, vh_});
    const double x0 = std::clamp(a.x, 0.0, iw_);
    const double y0 = std::clamp(a.y, 0.0, ih_);
    const double x1 = std::clamp(b.x, 0.0, iw_);
    const double y1 = std::clamp(b.y, 0.0, ih_);
    return Box{x0, y0, x1 - x0, y1 - y0};
}

Vec2 ImageViewport::view_to_image(Vec2 v) const noexcept {
    return Vec2{center_.x + ((v.x - (vw_ * 0.5)) / zoom_),
                center_.y + ((v.y - (vh_ * 0.5)) / zoom_)};
}

Vec2 ImageViewport::image_to_view(Vec2 i) const noexcept {
    return Vec2{((i.x - center_.x) * zoom_) + (vw_ * 0.5),
                ((i.y - center_.y) * zoom_) + (vh_ * 0.5)};
}

Box minimap_frame(double image_w, double image_h, double bx, double by, double max_w,
                  double max_h) noexcept {
    if (image_w <= 0.0 || image_h <= 0.0 || max_w <= 0.0 || max_h <= 0.0) {
        return Box{bx, by, 0.0, 0.0};
    }
    const double s = std::min(max_w / image_w, max_h / image_h);
    const double w = std::max(1.0, std::round(image_w * s));
    const double h = std::max(1.0, std::round(image_h * s));
    return Box{bx + max_w - w, by + max_h - h, w, h};
}

Box minimap_visible(const ImageViewport& viewport, const Box& frame) noexcept {
    if (viewport.empty() || frame.w <= 0.0) {
        return frame;
    }
    const Box v = viewport.visible_region();
    const double sx = frame.w / viewport.image_width();
    const double sy = frame.h / viewport.image_height();
    return Box{frame.x + (v.x * sx), frame.y + (v.y * sy), v.w * sx, v.h * sy};
}

Vec2 minimap_to_image(const ImageViewport& viewport, const Box& frame, Vec2 point) noexcept {
    if (viewport.empty() || frame.w <= 0.0 || frame.h <= 0.0) {
        return Vec2{};
    }
    const double fx = std::clamp((point.x - frame.x) / frame.w, 0.0, 1.0);
    const double fy = std::clamp((point.y - frame.y) / frame.h, 0.0, 1.0);
    return Vec2{fx * viewport.image_width(), fy * viewport.image_height()};
}

DecodeTarget bucket_target(double view_w, double view_h, std::uint32_t bucket) noexcept {
    if (bucket == 0) {
        bucket = 1;
    }
    const auto up = [bucket](double v) -> std::uint32_t {
        if (v <= 0.0) {
            return bucket;
        }
        const auto n = static_cast<std::uint64_t>(std::ceil(v / static_cast<double>(bucket)));
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(n * bucket, 65535U));
    };
    return DecodeTarget{up(view_w), up(view_h)};
}

bool covers(DecodeTarget have, DecodeTarget need) noexcept {
    if (have.width == 0 && have.height == 0) {
        return true; // full resolution serves every fit view
    }
    return have.width >= need.width && have.height >= need.height;
}

} // namespace navigator::ui::inspector
