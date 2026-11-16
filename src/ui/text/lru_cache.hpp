// src/ui/text/lru_cache.hpp
//
// Small fixed-capacity least-recently-used map. Used to keep shaped text between frames so an
// immediate-mode UI that re-submits the same strings every frame does not re-run HarfBuzz.
// Keys are std::string (callers pack binary prefixes plus the text), so lookups with a reused key
// buffer never allocate. Values are stored as shared_ptr<const V>: an entry evicted while a caller
// still holds it stays alive until the caller drops it.
// Thread-safety: none; one owner (the UI thread).
#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace navigator::ui::text {

template <typename V> class LruCache {
public:
    using Ptr = std::shared_ptr<const V>;

    explicit LruCache(std::size_t capacity) : capacity_(capacity > 0 ? capacity : 1) {}

    // Returns the cached value and marks it most recently used, or nullptr.
    [[nodiscard]] Ptr find(const std::string& key) {
        const auto it = index_.find(key);
        if (it == index_.end()) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        order_.splice(order_.begin(), order_, it->second);
        return it->second->second;
    }

    // Inserts (or replaces) `key`, evicting the least recently used entry when full.
    Ptr insert(const std::string& key, V value) {
        auto ptr = std::make_shared<const V>(std::move(value));
        if (const auto it = index_.find(key); it != index_.end()) {
            it->second->second = ptr;
            order_.splice(order_.begin(), order_, it->second);
            return ptr;
        }
        if (order_.size() >= capacity_) {
            index_.erase(order_.back().first);
            order_.pop_back();
        }
        order_.emplace_front(key, ptr);
        index_.emplace(key, order_.begin());
        return ptr;
    }

    void clear() noexcept {
        index_.clear();
        order_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::size_t misses() const noexcept { return misses_; }

private:
    using Entry = std::pair<std::string, Ptr>;
    std::size_t capacity_;
    std::list<Entry> order_; // most recent first
    std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

} // namespace navigator::ui::text
