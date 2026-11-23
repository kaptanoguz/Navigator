// src/ui/inspector/thumbnail_scheduler.cpp — list thumbnail requests. See thumbnail_scheduler.hpp.
#include "inspector/thumbnail_scheduler.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace navigator::ui::inspector {

using preview::RequestPriority;

void ThumbnailScheduler::begin_frame() noexcept {
    ++frame_;
    wanted_.clear();
    visible_hashes_.clear();
    any_fresh_ = false;
}

std::shared_ptr<const preview::PreviewResult>
ThumbnailScheduler::want(const preview::PreviewRequest& request, RequestPriority priority) {
    if (!enabled()) {
        return nullptr;
    }
    return want(preview_key(request), request, priority);
}

std::shared_ptr<const preview::PreviewResult>
ThumbnailScheduler::want(const preview::PreviewKey& key, const preview::PreviewRequest& request,
                         RequestPriority priority) {
    if (!enabled()) {
        return nullptr;
    }
    if (priority == RequestPriority::Visible) {
        visible_hashes_.insert(preview_key_hash(key));
    }
    auto [it, inserted] = slots_.try_emplace(key);
    Slot& s = it->second;
    s.last_frame = frame_;
    if (inserted || s.state == State::Fresh) {
        s.state = State::Fresh;
        s.request = request;
        s.priority = priority;
        any_fresh_ = true;
    } else if (s.state == State::Pending && priority < s.priority) {
        s.priority = priority; // raised on the next submit (resubmission coalesces)
    }
    wanted_.push_back(key);
    return s.state == State::Ready ? s.result : nullptr;
}

void ThumbnailScheduler::submit(const preview::PreviewKey& key, Slot& slot) {
    if (std::shared_ptr<const preview::PreviewResult> hit = service_->lookup(key); hit) {
        slot.state = State::Ready;
        slot.result = std::move(hit);
        slot.request = {};
        ++ready_serial_;
        return;
    }
    const preview::Ticket t = service_->submit(slot.request, slot.priority, epoch_);
    ++submitted_;
    if (!t.valid()) {
        slot.state = State::Failed;
        return;
    }
    if (slot.ticket.valid() && slot.ticket != t) {
        by_ticket_.erase(slot.ticket.value);
    }
    slot.ticket = t;
    slot.epoch = epoch_;
    slot.state = State::Pending;
    by_ticket_[t.value] = key;
}

void ThumbnailScheduler::end_frame() {
    if (!enabled()) {
        return;
    }
    const bool changed = visible_hashes_ != last_visible_hashes_;
    if (changed) {
        ++epoch_;
        for (const preview::PreviewKey& key : wanted_) {
            auto it = slots_.find(key);
            if (it == slots_.end()) {
                continue;
            }
            Slot& s = it->second;
            if (s.state == State::Fresh || (s.state == State::Pending && s.epoch != epoch_)) {
                submit(key, s);
            }
        }
        service_->cancel_older(RequestPriority::Visible, epoch_);
        service_->cancel_older(RequestPriority::Prefetch, epoch_);
        // Jobs of older epochs are gone: their slots go back to Fresh (resubmitted when wanted).
        for (auto& [key, s] : slots_) {
            if (s.state == State::Pending && s.epoch != epoch_) {
                by_ticket_.erase(s.ticket.value);
                s.ticket = {};
                s.state = State::Fresh;
                s.result = nullptr;
            }
        }
        last_visible_hashes_ = visible_hashes_;
    } else if (any_fresh_) {
        // Same rows on screen, new keys among the prefetched ones (or a file changed).
        for (const preview::PreviewKey& key : wanted_) {
            auto it = slots_.find(key);
            if (it != slots_.end() && it->second.state == State::Fresh) {
                submit(key, it->second);
            }
        }
    }
    if (frame_ % 64 == 0) {
        for (auto it = slots_.begin(); it != slots_.end();) {
            const Slot& s = it->second;
            if (frame_ - s.last_frame > kForgetFrames && s.state != State::Pending) {
                it = slots_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool ThumbnailScheduler::on_completion(const preview::Completion& completion) {
    const auto t = by_ticket_.find(completion.ticket.value);
    if (t == by_ticket_.end()) {
        return false;
    }
    const preview::PreviewKey key = t->second;
    by_ticket_.erase(t);
    const auto it = slots_.find(key);
    if (it == slots_.end()) {
        return true;
    }
    Slot& s = it->second;
    s.ticket = {};
    s.request = {};
    if (completion.result) {
        s.result = *completion.result;
        s.state = State::Ready;
        ++ready_serial_;
    } else {
        s.state = State::Failed;
    }
    return true;
}

void ThumbnailScheduler::forget(const Path& path) {
    const std::string& p = path.native();
    for (auto it = slots_.begin(); it != slots_.end();) {
        const std::string& k = it->first.path.native();
        const bool below = k == p || (k.size() > p.size() && k.starts_with(p) &&
                                      (p.ends_with('/') || k[p.size()] == '/'));
        if (below) {
            if (it->second.ticket.valid()) {
                if (service_ != nullptr) {
                    service_->cancel(it->second.ticket);
                }
                by_ticket_.erase(it->second.ticket.value);
            }
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
    last_visible_hashes_.clear(); // resubmit what is visible next frame
}

void ThumbnailScheduler::clear() noexcept {
    slots_.clear();
    by_ticket_.clear();
    last_visible_hashes_.clear();
}

std::size_t ThumbnailScheduler::pending_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(), [](const auto& kv) {
        return kv.second.state == State::Pending;
    }));
}

} // namespace navigator::ui::inspector
