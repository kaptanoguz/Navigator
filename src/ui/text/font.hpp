// src/ui/text/font.hpp
//
// Font discovery (fontconfig), faces (FreeType), shaping (HarfBuzz) and glyph rasterization to
// 8-bit coverage. No SDL and no FreeType/HarfBuzz types in this header (pimpl), so tests and the
// renderer-side atlas can use it freely.
// Thread-safety: a FontLibrary and the Fonts opened from it belong to one thread (the UI thread).
//                find_font() may be called from any thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>

#include "theme/theme.hpp"

namespace navigator::ui::text {

struct FontFile {
    std::string path;
    int index = 0; // FreeType face index (named-instance bits included for variable fonts)
    std::string family;
};

// UI family preference, best first. The last entry is the generic "sans-serif".
// NAVIGATOR_FONT_FAMILY (dev override) is tried before the list when set.
[[nodiscard]] std::vector<std::string> preferred_families();

// Finds the first family in `families` that fontconfig really has (not a substitute) and that
// covers Turkish letters. "sans-serif" accepts whatever fontconfig substitutes.
// Errors: NotFound (nothing usable), PlatformError (fontconfig failed to initialize).
[[nodiscard]] Result<FontFile> find_font(const std::vector<std::string>& families,
                                         FontWeight weight);

struct FontMetrics {
    float ascender = 0.0F;  // px above the baseline (positive)
    float descender = 0.0F; // px below the baseline (positive)
    float line_height = 0.0F;
    float cap_height = 0.0F;
    float x_height = 0.0F;
};

struct ShapedGlyph {
    std::uint32_t glyph = 0; // glyph index; 0 is .notdef
    float x = 0.0F;          // pen x + offset, px from run origin
    float y = 0.0F;          // offset, px, positive down
    std::uint32_t cluster = 0;
};

struct ShapedRun {
    std::vector<ShapedGlyph> glyphs;
    float advance = 0.0F; // total width, px
};

struct ShapeOptions {
    float tracking_em = 0.0F;
    bool tabular_figures = false;
    std::string_view language = "en"; // BCP 47; "tr" enables Turkish locl forms
};

struct GlyphBitmap {
    int width = 0;
    int height = 0;
    int left = 0;                       // px from pen x to the bitmap's left edge
    int top = 0;                        // px from baseline up to the bitmap's top edge
    std::vector<std::uint8_t> coverage; // width*height, row-major, top row first
};

class FontLibrary {
public:
    // Errors: PlatformError (FreeType initialization failed).
    [[nodiscard]] static Result<std::shared_ptr<FontLibrary>> create();
    ~FontLibrary();
    FontLibrary(const FontLibrary&) = delete;
    FontLibrary& operator=(const FontLibrary&) = delete;
    FontLibrary(FontLibrary&&) = delete;
    FontLibrary& operator=(FontLibrary&&) = delete;

    struct Impl;
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

private:
    explicit FontLibrary(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class Font {
public:
    // Opens `file` at `pixel_size` (physical pixels per em).
    // Errors: NotFound / IoError (cannot open the face), InvalidArgument (bad size).
    [[nodiscard]] static Result<std::unique_ptr<Font>> open(std::shared_ptr<FontLibrary> library,
                                                            const FontFile& file, float pixel_size);

    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&&) = delete;
    Font& operator=(Font&&) = delete;

    [[nodiscard]] std::uint32_t id() const noexcept; // unique per opened Font (cache key)
    [[nodiscard]] float pixel_size() const noexcept;
    [[nodiscard]] const FontMetrics& metrics() const noexcept;
    [[nodiscard]] const FontFile& file() const noexcept;

    [[nodiscard]] ShapedRun shape(std::string_view utf8, const ShapeOptions& options = {});

    // Renders `glyph` with its origin shifted right by `subpixel_x` (0..1 px). Light hinting:
    // vertical snapping only, so subpixel horizontal placement stays accurate.
    [[nodiscard]] Result<GlyphBitmap> rasterize(std::uint32_t glyph, float subpixel_x);

private:
    struct Impl;
    explicit Font(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace navigator::ui::text
