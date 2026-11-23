// src/ui/inspector/texture_cache.cpp — preview textures. See texture_cache.hpp.
#include "inspector/texture_cache.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace navigator::ui::inspector {

const preview::ImageBuffer* image_of(const preview::PreviewResult& result) noexcept {
    if (const auto* img = std::get_if<preview::ImagePreview>(&result.content)) {
        return img->image.valid() ? &img->image : nullptr;
    }
    if (const auto* vid = std::get_if<preview::VideoPreview>(&result.content)) {
        return vid->poster.valid() ? &vid->poster : nullptr;
    }
    return nullptr;
}

TextureCache::TextureCache(Renderer& renderer, std::size_t budget_bytes)
    : renderer_(&renderer),
      budget_(std::max<std::size_t>(budget_bytes, std::size_t{16} * 1024U * 1024U)) {}

TextureCache::~TextureCache() { clear(); }

void TextureCache::destroy(Texture& t) noexcept {
    for (Tile& tile : t.tiles) {
        Renderer::destroy_texture(tile.texture);
        tile.texture = nullptr;
    }
    t.tiles.clear();
}

void TextureCache::clear() {
    for (auto& [key, t] : textures_) {
        destroy(t);
        queue_.remove(t.id);
    }
    textures_.clear();
    by_id_.clear();
    bytes_ = 0;
}

const TextureCache::Texture* TextureCache::get(const preview::PreviewKey& key) {
    const auto it = textures_.find(key);
    if (it == textures_.end()) {
        return nullptr;
    }
    it->second.last_frame = frame_;
    return it->second.complete ? &it->second : nullptr;
}

void TextureCache::request(const preview::PreviewKey& key,
                           std::shared_ptr<const preview::PreviewResult> result, UploadClass cls) {
    if (!result || textures_.find(key) != textures_.end()) {
        return;
    }
    const preview::ImageBuffer* img = image_of(*result);
    if (img == nullptr) {
        return;
    }
    Texture t;
    t.width = static_cast<int>(img->width);
    t.height = static_cast<int>(img->height);
    t.id = next_id_++;
    t.last_frame = frame_;
    // Tiles of at most the maximum texture side.
    int max_side = renderer_->max_texture_size();
    if (max_side <= 0) {
        max_side = 8192;
    }
    for (int y = 0; y < t.height; y += max_side) {
        for (int x = 0; x < t.width; x += max_side) {
            Tile tile;
            tile.x = x;
            tile.y = y;
            tile.w = std::min(max_side, t.width - x);
            tile.h = std::min(max_side, t.height - y);
            tile.texture = renderer_->create_image_texture(tile.w, tile.h);
            if (tile.texture == nullptr) {
                destroy(t);
                return;
            }
            t.tiles.push_back(tile);
        }
    }
    t.bytes = static_cast<std::size_t>(img->stride) * img->height;
    t.source = std::move(result);
    bytes_ += t.bytes;
    queue_.push(t.id, cls, img->height, img->stride, t.id);
    by_id_[t.id] = key;
    textures_.emplace(key, std::move(t));
}

bool TextureCache::upload(double now_ms, std::size_t budget) {
    if (queue_.empty()) {
        return false;
    }
    bool landed = false;
    for (const auto& slice : queue_.plan(budget)) {
        const auto k = by_id_.find(slice.id);
        if (k == by_id_.end()) {
            continue;
        }
        const auto it = textures_.find(k->second);
        if (it == textures_.end() || !it->second.source) {
            continue;
        }
        Texture& t = it->second;
        const preview::ImageBuffer* img = image_of(*t.source);
        if (img == nullptr) {
            continue;
        }
        const int y0 = static_cast<int>(slice.first_row);
        const int y1 = y0 + static_cast<int>(slice.row_count);
        for (const Tile& tile : t.tiles) {
            const int a = std::max(y0, tile.y);
            const int b = std::min(y1, tile.y + tile.h);
            if (a >= b) {
                continue;
            }
            const std::uint8_t* src = img->pixels.data() +
                                      (static_cast<std::size_t>(a) * img->stride) +
                                      (static_cast<std::size_t>(tile.x) * 4U);
            // Rows relative to the tile.
            (void)renderer_->update_image_texture(tile.texture, a - tile.y, b - a, src,
                                                  static_cast<int>(img->stride));
        }
        if (slice.last) {
            t.complete = true;
            t.ready_ms = now_ms;
            t.source.reset(); // the service's cache keeps the pixels as long as it wants
            landed = true;
        }
    }
    return landed;
}

void TextureCache::end_frame() {
    if (bytes_ > budget_) {
        std::vector<std::pair<std::uint64_t, preview::PreviewKey>> order;
        order.reserve(textures_.size());
        for (const auto& [key, t] : textures_) {
            if (t.last_frame < frame_) {
                order.emplace_back(t.last_frame, key);
            }
        }
        std::sort(order.begin(), order.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [frame, key] : order) {
            if (bytes_ <= budget_) {
                break;
            }
            const auto it = textures_.find(key);
            if (it == textures_.end()) {
                continue;
            }
            bytes_ -= std::min(bytes_, it->second.bytes);
            queue_.remove(it->second.id);
            by_id_.erase(it->second.id);
            destroy(it->second);
            textures_.erase(it);
        }
    }
    ++frame_;
}

void TextureCache::draw(Renderer& r, const Texture& tex, const RectF& dst, float alpha,
                        bool nearest) {
    if (tex.width <= 0 || tex.height <= 0) {
        return;
    }
    const float sx = dst.w / static_cast<float>(tex.width);
    const float sy = dst.h / static_cast<float>(tex.height);
    for (const Tile& tile : tex.tiles) {
        const RectF d{dst.x + (static_cast<float>(tile.x) * sx),
                      dst.y + (static_cast<float>(tile.y) * sy), static_cast<float>(tile.w) * sx,
                      static_cast<float>(tile.h) * sy};
        r.draw_image(tile.texture, d, alpha, nearest);
    }
}

} // namespace navigator::ui::inspector
