// tests/ui/fake_preview_service.hpp — a scripted IPreviewService for the ui's Phase 6 tests:
// records submissions, cancellations and cancel_older calls, coalesces identical pending keys
// like the real service, and lets the test complete jobs and seed the memory cache.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>

#include "inspector/preview_backend.hpp"

namespace navigator::ui::test {

class FakePreviewService final : public preview::IPreviewService {
public:
    struct Job {
        preview::Ticket ticket;
        preview::PreviewRequest request;
        preview::PreviewKey key;
        preview::RequestPriority priority = preview::RequestPriority::Visible;
        std::uint64_t epoch = 0;
    };

    preview::Ticket submit(preview::PreviewRequest request, preview::RequestPriority priority,
                           std::uint64_t epoch) override {
        ++submits;
        const preview::PreviewKey key = preview_key(request);
        for (Job& j : pending) {
            if (j.key == key) { // coalescing
                if (priority < j.priority) {
                    j.priority = priority;
                }
                j.epoch = std::max(j.epoch, epoch);
                return j.ticket;
            }
        }
        Job j;
        j.ticket = preview::Ticket{next_++};
        j.request = std::move(request);
        j.key = key;
        j.priority = priority;
        j.epoch = epoch;
        pending.push_back(j);
        return j.ticket;
    }

    [[nodiscard]] std::shared_ptr<const preview::PreviewResult>
    lookup(const preview::PreviewKey& key) const override {
        ++lookups;
        const auto it = cache.find(key);
        return it == cache.end() ? nullptr : it->second;
    }

    void cancel(preview::Ticket ticket) noexcept override {
        ++cancels;
        std::erase_if(pending, [ticket](const Job& j) { return j.ticket == ticket; });
    }

    void cancel_older(preview::RequestPriority priority, std::uint64_t epoch) noexcept override {
        older_calls.emplace_back(priority, epoch);
        std::erase_if(pending,
                      [&](const Job& j) { return j.priority == priority && j.epoch < epoch; });
    }

    std::size_t poll(std::vector<preview::Completion>& out) override {
        const std::size_t n = done.size();
        for (preview::Completion& c : done) {
            out.push_back(std::move(c));
        }
        done.clear();
        return n;
    }

    void invalidate(const Path& /*path*/, preview::InvalidateMode /*mode*/) override {
        ++invalidations;
    }
    void set_cache_budget(std::uint64_t /*bytes*/) override {}
    void clear_cache() noexcept override { cache.clear(); }
    [[nodiscard]] preview::ServiceStats stats() const override { return {}; }
    void shutdown() noexcept override {}

    // Test helpers ---------------------------------------------------------------------------
    // Finishes the pending job with this key (success with `result`), as a completion.
    bool finish(const preview::PreviewKey& key,
                std::shared_ptr<const preview::PreviewResult> result) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->key == key) {
                cache[key] = result;
                done.push_back(preview::Completion{it->ticket, key, result});
                pending.erase(it);
                return true;
            }
        }
        return false;
    }
    bool fail(const preview::PreviewKey& key, ErrorCode code) {
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->key == key) {
                done.push_back(preview::Completion{it->ticket, key, navigator::fail(code, "fake")});
                pending.erase(it);
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] const Job* find(const preview::PreviewKey& key) const {
        for (const Job& j : pending) {
            if (j.key == key) {
                return &j;
            }
        }
        return nullptr;
    }

    std::deque<Job> pending;
    std::vector<preview::Completion> done;
    std::unordered_map<preview::PreviewKey, std::shared_ptr<const preview::PreviewResult>,
                       UiPreviewKeyHash>
        cache;
    std::vector<std::pair<preview::RequestPriority, std::uint64_t>> older_calls;
    std::uint64_t submits = 0;
    mutable std::uint64_t lookups = 0;
    std::uint64_t cancels = 0;
    std::uint64_t invalidations = 0;

private:
    std::uint64_t next_ = 1;
};

// A small image result (premultiplied RGBA, opaque grey).
inline std::shared_ptr<const preview::PreviewResult> image_result(std::uint32_t w,
                                                                  std::uint32_t h) {
    auto r = std::make_shared<preview::PreviewResult>();
    preview::ImagePreview img;
    img.image.width = w;
    img.image.height = h;
    img.image.stride = w * 4;
    img.image.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0x80);
    img.image.opaque = true;
    img.info.format = preview::ImageFormat::Png;
    img.info.stored = {w, h};
    img.info.display = {w, h};
    r->content = std::move(img);
    r->provider_id = "fake";
    return r;
}

inline fs::DirEntry file_entry(std::string name, std::uint64_t size = 1000) {
    fs::DirEntry e;
    e.name = std::move(name);
    e.kind = fs::EntryKind::File;
    e.size = size;
    e.mtime = Timestamp{std::chrono::seconds{1'700'000'000}};
    e.mode = 0100644;
    e.readable = true;
    return e;
}

} // namespace navigator::ui::test
