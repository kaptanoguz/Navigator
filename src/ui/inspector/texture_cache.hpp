// src/ui/inspector/texture_cache.hpp
//
// Phase 6 GPU textures of preview pixels (Inspector images, video posters, list thumbnails).
// Finished results are uploaded on the UI thread at frame start through an UploadQueue: at most
// kUploadBudgetBytes per frame, the Inspector first, big images in row bands over several frames
// (the previous texture stays on screen meanwhile), so a burst of thumbnails or a 32 MP decode
// never stalls a frame. Images larger than the renderer's maximum texture size are split into
// tiles. Textures are an LRU bounded by a byte budget; textures drawn this frame are never
// evicted.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <navigator/preview.hpp>

#include "inspector/preview_backend.hpp"
#include "inspector/upload_queue.hpp"
#include "render/renderer.hpp"

namespace navigator::ui::inspector {

inline constexpr std::size_t kDefaultTextureBudgetBytes = std::size_t{256} * 1024U * 1024U;

// Pixels of a result that become a texture: the image, or the video poster; nullptr otherwise.
[[nodiscard]] const preview::ImageBuffer* image_of(const preview::PreviewResult& result) noexcept;

class TextureCache {
public:
    struct Tile {
        SDL_Texture* texture = nullptr;
        int x = 0; // px within the image
        int y = 0;
        int w = 0;
        int h = 0;
    };
    struct Texture {
        std::vector<Tile> tiles;
        int width = 0; // px of the uploaded buffer
        int height = 0;
        std::size_t bytes = 0;
        bool complete = false;
        double ready_ms = -1.0; // when the last band landed (fade-in start)
        std::uint64_t last_frame = 0;
        std::uint64_t id = 0;
        std::shared_ptr<const preview::PreviewResult> source; // released once complete
    };

    TextureCache(Renderer& renderer, std::size_t budget_bytes);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    TextureCache(TextureCache&&) = delete;
    TextureCache& operator=(TextureCache&&) = delete;

    // A complete texture for `key` (touched: kept this frame), or nullptr.
    [[nodiscard]] const Texture* get(const preview::PreviewKey& key);
    // Queues an upload of `result`'s pixels for `key` unless present (no-op without pixels).
    void request(const preview::PreviewKey& key,
                 std::shared_ptr<const preview::PreviewResult> result, UploadClass cls);
    [[nodiscard]] bool contains(const preview::PreviewKey& key) const noexcept {
        return textures_.find(key) != textures_.end();
    }
    // Frame start: uploads within the budget. Returns true when anything landed.
    bool upload(double now_ms, std::size_t budget = kUploadBudgetBytes);
    // Frame end: evicts least recently drawn textures over budget.
    void end_frame();
    [[nodiscard]] bool uploading() const noexcept { return !queue_.empty(); }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t count() const noexcept { return textures_.size(); }
    void clear();

    // Draws `tex` into `dst` (px; the whole buffer maps onto dst).
    static void draw(Renderer& r, const Texture& tex, const RectF& dst, float alpha, bool nearest);

private:
    void destroy(Texture& t) noexcept;

    Renderer* renderer_;
    std::size_t budget_;
    std::size_t bytes_ = 0;
    std::uint64_t frame_ = 1;
    std::uint64_t next_id_ = 1;
    std::unordered_map<preview::PreviewKey, Texture, UiPreviewKeyHash> textures_;
    std::unordered_map<std::uint64_t, preview::PreviewKey> by_id_;
    UploadQueue<std::uint64_t> queue_; // payload unused (id is the job id)
};

} // namespace navigator::ui::inspector
