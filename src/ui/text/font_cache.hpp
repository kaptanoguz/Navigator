// src/ui/text/font_cache.hpp
//
// The UI font set at the current display scale: the UI family (Regular, Medium, SemiBold) plus
// fontconfig fallbacks for codepoints the family lacks, and a cache of shaped text.
//
// Shaping with fallback: the string is itemized into runs by font coverage (UI face first; then
// the font the current run already uses; then FontFallback, memoized per codepoint). Combining
// marks, joiners and variation selectors stay in the preceding run. Each run is shaped by
// HarfBuzz with the whole string as context. Runs are laid out left to right in logical order:
// there is no bidi reordering yet, so an RTL run (Arabic, Hebrew) is shaped correctly on its own
// but mixed-direction strings are not visually reordered. A codepoint no installed font covers
// falls back to the UI face (.notdef).
//
// Shape cache: an LRU keyed by (weight, pixel size, tracking, tnum, language, text) holds the
// last kShapeCacheCapacity layouts, so per-frame re-submission of the same strings costs a hash
// lookup. clear() (display scale change) drops fonts and layouts together.
// No SDL here, so tests can shape without a window.
// Thread-safety: UI thread only.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>

#include "text/font.hpp"
#include "text/lru_cache.hpp"
#include "theme/theme.hpp"

namespace navigator::ui::text {

// A shaped single-line string, possibly spanning several fonts.
struct TextLayout {
    struct Run {
        Font* font = nullptr; // owned by the FontCache that produced the layout
        ShapedRun shaped;     // glyph x positions are relative to `x`
        float x = 0.0F;       // px from the layout origin
    };
    std::vector<Run> runs;
    float advance = 0.0F;    // px
    float cap_height = 0.0F; // of the primary (UI) face, px
};

class FontCache {
public:
    static constexpr std::size_t kShapeCacheCapacity = 2048;

    // Discovers the UI family (Regular, Medium, SemiBold faces). Errors: NotFound (no usable
    // font: the app refuses to start with a clear message), PlatformError.
    [[nodiscard]] static Result<std::unique_ptr<FontCache>> create();

    // UI face for `style` at `scale` (px per dp). Errors from Font::open.
    [[nodiscard]] Result<Font*> get(const TextStyle& style, float scale);

    // Shapes `utf8` with fallback fonts, from the cache when possible. Returns an empty layout
    // when the UI face cannot be opened. The layout's Font pointers stay valid until clear().
    [[nodiscard]] std::shared_ptr<const TextLayout>
    shape(const TextStyle& style, float scale, std::string_view utf8, std::string_view language);

    // Drops opened fonts and cached layouts (call when the display scale changes).
    void clear();

    [[nodiscard]] const FontFile& regular_file() const noexcept { return files_[0]; }
    [[nodiscard]] const LruCache<TextLayout>& shape_cache() const noexcept { return layouts_; }

private:
    FontCache() : layouts_(kShapeCacheCapacity) {}

    struct Slot {
        const FontFile* file;
        int size_q; // pixel size in 1/8 px
        std::unique_ptr<Font> font;
    };

    Result<Font*> open(const FontFile& file, int size_q);
    Font* fallback_font(char32_t codepoint, FontWeight weight, int size_q);
    TextLayout layout(const TextStyle& style, Font& primary, int size_q, std::string_view utf8,
                      std::string_view language);

    std::shared_ptr<FontLibrary> library_;
    std::array<FontFile, 3> files_; // Regular, Medium, SemiBold
    std::array<std::unique_ptr<FontFallback>, 3> fallbacks_;
    std::array<bool, 3> fallback_tried_{};
    std::vector<Slot> fonts_;
    LruCache<TextLayout> layouts_;
    std::string key_; // reused lookup key buffer
};

// Exposed for tests: decodes one UTF-8 codepoint at `pos` and advances it. Invalid or truncated
// sequences decode to U+FFFD and advance by one byte.
[[nodiscard]] char32_t decode_utf8(std::string_view s, std::size_t& pos) noexcept;

} // namespace navigator::ui::text
