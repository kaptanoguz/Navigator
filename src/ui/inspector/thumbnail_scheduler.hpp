// src/ui/inspector/thumbnail_scheduler.hpp
//
// Phase 6 list thumbnails (docs/architecture.md "Phase 6" data flow; design review "Thumbnails in
// list and grid"): which thumbnail requests go to the IPreviewService, with which priority and
// epoch, and which results are ready to draw. Headless (no SDL): tests drive it with a fake
// service.
//
// Per frame:
//   begin_frame()                 starts collecting
//   want(request, priority)       for each visible row needing a thumbnail (Visible) and a few
//                                 rows just off screen (Prefetch); returns the finished result
//                                 or nullptr (the row keeps its type icon)
//   end_frame()                   when the wanted Visible set differs from the previous frame
//                                 (scroll, navigation, tab switch) the epoch is bumped; new keys
//                                 are submitted (after a memory-cache lookup), keys still pending
//                                 are resubmitted with the new epoch (coalescing refreshes them),
//                                 then cancel_older(Visible / Prefetch, epoch) drops every job
//                                 that left the screen. Without a change nothing is resubmitted,
//                                 so a still frame costs one hash probe per row.
//   on_completion(completion)     routes a polled Completion of one of our tickets: the result is
//                                 kept for drawing; an error marks the key failed (icon stays,
//                                 never retried until the file changes, which is a new key).
// Slots of keys not wanted for kForgetFrames frames are dropped (results stay in the service's
// LRU), bounding memory to what was recently on screen.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>

#include "inspector/preview_backend.hpp"

namespace navigator::ui::inspector {

inline constexpr std::uint64_t kForgetFrames = 600;
// Rows past the last visible row that are prefetched (and as many before the first).
inline constexpr std::size_t kThumbnailPrefetchRows = 8;

class ThumbnailScheduler {
public:
    // `service` may be null (no previews in this build): want() then always returns nullptr.
    explicit ThumbnailScheduler(preview::IPreviewService* service) noexcept : service_(service) {}

    void set_service(preview::IPreviewService* service) noexcept { service_ = service; }
    [[nodiscard]] bool enabled() const noexcept { return service_ != nullptr && enabled_; }
    // --no-thumbnails: want() returns nullptr and submits nothing.
    void set_enabled(bool on) noexcept { enabled_ = on; }

    void begin_frame() noexcept;
    // `request.purpose` must be Thumbnail. Priority Visible or Prefetch.
    [[nodiscard]] std::shared_ptr<const preview::PreviewResult>
    want(const preview::PreviewRequest& request, preview::RequestPriority priority);
    // As want(), with the key already computed (preview_key(request)).
    [[nodiscard]] std::shared_ptr<const preview::PreviewResult>
    want(const preview::PreviewKey& key, const preview::PreviewRequest& request,
         preview::RequestPriority priority);
    void end_frame();

    // True when the completion was ours (consumed).
    bool on_completion(const preview::Completion& completion);
    // A watch event / F5: forget the slots below `path` (the service is invalidated separately).
    void forget(const Path& path);
    void clear() noexcept;

    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t pending_count() const noexcept;
    // Submissions made so far (tests, diagnostics).
    [[nodiscard]] std::uint64_t submitted() const noexcept { return submitted_; }
    // Key of a ready result changed since the given serial (the view repaints / starts a fade).
    [[nodiscard]] std::uint64_t ready_serial() const noexcept { return ready_serial_; }

private:
    enum class State : std::uint8_t { Fresh, Pending, Ready, Failed };
    struct Slot {
        State state = State::Fresh;
        preview::Ticket ticket;
        preview::RequestPriority priority = preview::RequestPriority::Visible;
        std::uint64_t epoch = 0;
        std::uint64_t last_frame = 0;
        preview::PreviewRequest request; // kept until submitted
        std::shared_ptr<const preview::PreviewResult> result;
    };

    void submit(const preview::PreviewKey& key, Slot& slot);

    preview::IPreviewService* service_ = nullptr;
    bool enabled_ = true;
    std::uint64_t frame_ = 0;
    std::uint64_t epoch_ = 1;
    std::uint64_t submitted_ = 0;
    std::uint64_t ready_serial_ = 0;
    std::unordered_map<preview::PreviewKey, Slot, UiPreviewKeyHash> slots_;
    std::unordered_map<std::uint64_t, preview::PreviewKey> by_ticket_;
    // This frame's wanted keys (Visible and Prefetch) and the previous frame's Visible set.
    std::vector<preview::PreviewKey> wanted_;
    std::unordered_set<std::size_t> visible_hashes_;
    std::unordered_set<std::size_t> last_visible_hashes_;
    bool any_fresh_ = false;
};

} // namespace navigator::ui::inspector
