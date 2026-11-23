// src/ui/inspector/inspector_controller.cpp — Inspector logic. See inspector_controller.hpp.
#include "inspector/inspector_controller.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <navigator/mime.hpp>

#include "inspector/preview_backend.hpp"

namespace navigator::ui::inspector {

using preview::PreviewPurpose;
using preview::RequestPriority;

bool is_visual_mime(std::string_view mime) noexcept {
    return mime.starts_with("image/") || mime.starts_with("video/");
}

void Controller::toggle_lock() noexcept {
    if (locked_) {
        locked_ = false;
        return;
    }
    if (item_ && selected_count_ <= 1) {
        locked_ = true;
    }
}

bool Controller::unlock_if_gone(bool exists) noexcept {
    if (locked_ && !exists) {
        locked_ = false;
        return true;
    }
    return false;
}

void Controller::cancel() noexcept {
    if (service_ != nullptr && ticket_.valid()) {
        service_->cancel(ticket_);
    }
    ticket_ = {};
    for (const preview::Ticket t : prefetch_) {
        if (service_ != nullptr) {
            service_->cancel(t);
        }
    }
    prefetch_.clear();
}

void Controller::set_visible(bool visible, double now_ms) {
    if (visible == visible_) {
        return;
    }
    visible_ = visible;
    if (!visible) {
        cancel();
        due_ms_ = -1.0;
        want_key_.reset(); // requested again when shown
        return;
    }
    // Shown: the current item (if any) is requested at once, no debounce.
    if (item_) {
        request(now_ms, true);
    }
    ++serial_;
}

preview::PreviewRequest Controller::build_request(const Item& item) const {
    const std::string_view mime = mime::guess_for_entry(item.entry);
    preview::PixelSize target{};
    if (is_visual_mime(mime) && !full_resolution_) {
        target = fit_target_;
    }
    preview::PreviewRequest r =
        make_request(item.path, item.entry, PreviewPurpose::Inspector, target);
    r.location = item.location;
    r.folder = item.folder;
    return r;
}

void Controller::follow(const Subject& subject, double now_ms) {
    neighbours_ = subject.neighbours;
    if (locked_) {
        return;
    }
    const std::size_t count = subject.selected_count;
    if (count != selected_count_ || subject.selected_bytes != selected_bytes_) {
        const bool was_multi = selected_count_ > 1;
        selected_count_ = count;
        selected_bytes_ = subject.selected_bytes;
        if (was_multi != (count > 1)) {
            ++serial_;
        }
    }
    if (!subject.cursor) {
        if (item_) {
            cancel();
            item_.reset();
            result_.reset();
            result_key_.reset();
            want_key_.reset();
            result_current_ = false;
            error_.reset();
            due_ms_ = -1.0;
            ++serial_;
        }
        return;
    }
    if (item_ && item_->same_version(*subject.cursor)) {
        // Same file; view options of a folder may differ (hidden / sort) -> new key.
        if (!(item_->folder == subject.cursor->folder) ||
            !(item_->location == subject.cursor->location)) {
            item_ = *subject.cursor;
            request(now_ms, true);
        }
        return;
    }
    const bool refresh = item_ && item_->path == subject.cursor->path; // changed on disk
    adopt(*subject.cursor, now_ms, refresh);
}

void Controller::adopt(Item item, double now_ms, bool refresh) {
    const bool rapid = now_ms - last_change_ms_ < kFollowDebounceMs;
    last_change_ms_ = now_ms;
    cancel();
    item_ = std::move(item);
    error_.reset();
    prefetched_ = false;
    if (!refresh) {
        result_.reset();
        result_key_.reset();
    }
    result_current_ = false;
    want_key_.reset();
    ++serial_;
    // The first change after a pause goes out at once; a run of changes (held arrow key) waits
    // until it settles.
    request(now_ms, !rapid);
}

void Controller::request(double now_ms, bool immediate) {
    if (!item_ || !visible_) {
        return;
    }
    const preview::PreviewRequest r = build_request(*item_);
    const preview::PreviewKey key = preview_key(r);
    if (want_key_ && *want_key_ == key &&
        (ticket_.valid() || result_current_ || error_ || due_ms_ >= 0.0)) {
        return;
    }
    want_key_ = key;
    if (result_key_ && *result_key_ == key && result_) {
        result_current_ = true;
        due_ms_ = -1.0;
        return;
    }
    if (service_ != nullptr) {
        if (std::shared_ptr<const preview::PreviewResult> hit = service_->lookup(key); hit) {
            if (ticket_.valid()) {
                service_->cancel(ticket_);
                ticket_ = {};
            }
            result_ = std::move(hit);
            result_key_ = key;
            result_current_ = true;
            error_.reset();
            due_ms_ = -1.0;
            ++serial_;
            return;
        }
    }
    due_ms_ = immediate ? now_ms : now_ms + kFollowDebounceMs;
}

void Controller::submit_now() {
    due_ms_ = -1.0;
    if (!item_ || service_ == nullptr || !visible_) {
        if (service_ == nullptr && item_) {
            error_ = Error{ErrorCode::Unsupported, "no preview service"};
            ++serial_;
        }
        return;
    }
    if (ticket_.valid()) {
        service_->cancel(ticket_);
    }
    const preview::PreviewRequest r = build_request(*item_);
    ticket_ = service_->submit(r, RequestPriority::Inspector, 0);
    ++submitted_;
    if (!ticket_.valid()) {
        error_ = Error{ErrorCode::InvalidArgument, "request refused"};
        ++serial_;
    }
}

void Controller::set_view_target(preview::PixelSize fit_target, bool full_resolution,
                                 double now_ms) {
    const bool grew =
        fit_target.width > fit_target_.width || fit_target.height > fit_target_.height;
    const bool mode_changed = full_resolution != full_resolution_;
    if (!grew && !mode_changed) {
        return; // shrinking scales on the GPU
    }
    if (grew) {
        fit_target_ = preview::PixelSize{std::max(fit_target.width, fit_target_.width),
                                         std::max(fit_target.height, fit_target_.height)};
    }
    full_resolution_ = full_resolution;
    if (!item_ || !is_visual_mime(mime::guess_for_entry(item_->entry))) {
        return;
    }
    // A 1:1 result also serves fit; going back to fit keeps it.
    if (!full_resolution && result_key_ && result_key_->target.empty() && result_) {
        return;
    }
    result_current_ = false;
    request(now_ms, true);
}

void Controller::tick(double now_ms, std::uint64_t epoch) {
    if (due_ms_ >= 0.0 && now_ms >= due_ms_) {
        submit_now();
    }
    // Prefetch neighbours once the inspected image is on screen.
    if (!prefetched_ && result_current_ && service_ != nullptr && item_ && visible_ &&
        is_visual_mime(mime::guess_for_entry(item_->entry))) {
        prefetched_ = true;
        std::size_t n = 0;
        for (const Item& nb : neighbours_) {
            if (n >= 2 * kPrefetchNeighbours) {
                break;
            }
            if (!is_visual_mime(mime::guess_for_entry(nb.entry))) {
                continue;
            }
            const preview::PreviewRequest r = build_request(nb);
            if (service_->lookup(preview_key(r))) {
                continue;
            }
            const preview::Ticket t = service_->submit(r, RequestPriority::Prefetch, epoch);
            if (t.valid()) {
                prefetch_.push_back(t);
            }
            ++n;
        }
    }
}

bool Controller::on_completion(const preview::Completion& completion) {
    const auto pf = std::find(prefetch_.begin(), prefetch_.end(), completion.ticket);
    if (pf != prefetch_.end()) {
        prefetch_.erase(pf);
        return true; // cached by the service; nothing to show yet
    }
    if (!ticket_.valid() || completion.ticket != ticket_) {
        return false;
    }
    ticket_ = {};
    if (completion.result) {
        result_ = *completion.result;
        result_key_ = completion.key;
        result_current_ = true;
        error_.reset();
    } else {
        error_ = completion.result.error();
        result_current_ = false;
        if (!(result_key_ && item_ && result_key_->path == item_->path)) {
            result_.reset();
        }
    }
    ++serial_;
    return true;
}

void Controller::invalidate() noexcept {
    want_key_.reset();
    result_current_ = false;
    ++serial_;
}

Status Controller::status() const noexcept {
    if (selected_count_ > 1 && !locked_) {
        return Status::Multi;
    }
    if (!item_) {
        return Status::Empty;
    }
    if (error_) {
        return Status::Failed;
    }
    if (result_ && result_current_) {
        return Status::Ready;
    }
    return Status::Loading;
}

// ---- Dock geometry --------------------------------------------------------------------------

layout::InspectorState clamped(layout::InspectorState state) noexcept {
    state.width =
        std::clamp(std::isfinite(state.width) ? state.width : layout::kInspectorDefaultWidth,
                   layout::kInspectorMinWidth, layout::kInspectorMaxWidth);
    state.height =
        std::clamp(std::isfinite(state.height) ? state.height : layout::kInspectorDefaultHeight,
                   layout::kInspectorMinHeight, layout::kInspectorMaxHeight);
    return state;
}

namespace {

constexpr double kAbsoluteMinExtent = 120.0; // dp

float fit_extent(double wanted_dp, double min_dp, double max_dp, float room_px, float scale) {
    const double room_dp = static_cast<double>(room_px) / static_cast<double>(scale);
    double e = std::clamp(wanted_dp, min_dp, max_dp);
    // The list keeps kInspectorMinListExtent; the Inspector shrinks first.
    e = std::min(e, room_dp - layout::kInspectorMinListExtent);
    e = std::max(e, std::min(kAbsoluteMinExtent, room_dp * 0.5));
    return std::round(static_cast<float>(e) * scale);
}

} // namespace

DockLayout dock_inspector(const layout::InspectorState& state, const RectF& content, float scale,
                          double window_w_dp, double window_h_dp) {
    DockLayout out;
    out.placement = layout::resolve_inspector_placement(state.placement, window_w_dp, window_h_dp);
    out.list = content;
    if (!state.visible || scale <= 0.0F) {
        return out;
    }
    const float grip = std::round(6.0F * scale);
    if (out.placement == layout::InspectorPlacement::Right) {
        const float w = fit_extent(state.width, layout::kInspectorMinWidth,
                                   layout::kInspectorMaxWidth, content.w, scale);
        out.inspector = RectF{content.right() - w, content.y, w, content.h};
        out.list = RectF{content.x, content.y, content.w - w, content.h};
        out.splitter = RectF{out.inspector.x - (0.5F * grip), content.y, grip, content.h};
    } else {
        const float h = fit_extent(state.height, layout::kInspectorMinHeight,
                                   layout::kInspectorMaxHeight, content.h, scale);
        out.inspector = RectF{content.x, content.bottom() - h, content.w, h};
        out.list = RectF{content.x, content.y, content.w, content.h - h};
        out.splitter = RectF{content.x, out.inspector.y - (0.5F * grip), content.w, grip};
    }
    return out;
}

double drag_extent(layout::InspectorPlacement placement, const RectF& content, float scale,
                   float pointer_px) noexcept {
    if (scale <= 0.0F) {
        return placement == layout::InspectorPlacement::Bottom ? layout::kInspectorDefaultHeight
                                                               : layout::kInspectorDefaultWidth;
    }
    if (placement == layout::InspectorPlacement::Bottom) {
        const double h = static_cast<double>(content.bottom() - pointer_px) / scale;
        return std::clamp(h, layout::kInspectorMinHeight, layout::kInspectorMaxHeight);
    }
    const double w = static_cast<double>(content.right() - pointer_px) / scale;
    return std::clamp(w, layout::kInspectorMinWidth, layout::kInspectorMaxWidth);
}

} // namespace navigator::ui::inspector
