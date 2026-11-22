// src/ui/widgets/text_fit.hpp
//
// Fits a single-line string into a pixel width with an ellipsis. Cuts only at shaping cluster
// boundaries (never inside a combining sequence). Middle elision keeps the file extension
// ("a very long na…me.tar.gz" style), as file managers do (design review P2-2).
// Fitted results are cached (LRU) by style, width and text, so a scrolled list re-uses them.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "shell/painter.hpp"
#include "text/lru_cache.hpp"

namespace navigator::ui {

enum class Elide : std::uint8_t { End, Middle };

class TextFitter {
public:
    explicit TextFitter(std::size_t capacity = 1024) : cache_(capacity) {}

    // `text` shaped to fit `max_px`; the full text when it already fits.
    [[nodiscard]] ShapedText fit(const Painter& painter, const TextStyle& style,
                                 std::string_view text, float max_px, Elide mode);

    // As fit(), plus the string that was shaped (Phase 5: match highlights are mapped onto the
    // elided text). `shown` is empty when the full text fits (then it is `text` itself).
    struct Fitted {
        ShapedText shaped;
        std::string shown;
    };
    [[nodiscard]] Fitted fit_text(const Painter& painter, const TextStyle& style,
                                  std::string_view text, float max_px, Elide mode);

    // The string fit() would shape (uncached); exposed for tests.
    [[nodiscard]] static std::string elide(const Painter& painter, const TextStyle& style,
                                           std::string_view text, float max_px, Elide mode);

    // Drop cached fits (display scale or font change).
    void clear() noexcept { cache_.clear(); }

private:
    text::LruCache<Fitted> cache_;
    std::string key_;
};

} // namespace navigator::ui
