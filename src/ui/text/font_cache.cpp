// src/ui/text/font_cache.cpp — UI font set, fallback itemization and shape cache. See
// font_cache.hpp.
#include "text/font_cache.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

namespace navigator::ui::text {
namespace {

int quantize_size(float px) noexcept { return static_cast<int>(std::lround(px * 8.0F)); }

// Codepoints that belong to the preceding run whatever font covers them: combining marks,
// joiners and variation selectors (approximate ranges; there is no Unicode database here).
bool attaches_to_previous(char32_t c) noexcept {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
           (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
           (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200C || c == 0x200D ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF);
}

// Spaces stay in the current run when its font has them, so "日本 語" is one run, not three.
bool is_space(char32_t c) noexcept { return c == 0x20 || c == 0xA0 || c == 0x3000; }

void append_bytes(std::string& out, const void* data, std::size_t n) {
    out.append(static_cast<const char*>(data), n);
}

} // namespace

char32_t decode_utf8(std::string_view s, std::size_t& pos) noexcept {
    constexpr char32_t kReplacement = 0xFFFD;
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    const unsigned char b0 = byte(pos);
    if (b0 < 0x80) {
        ++pos;
        return b0;
    }
    int extra = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if ((b0 & 0xE0U) == 0xC0U) {
        extra = 1;
        cp = b0 & 0x1FU;
        min = 0x80;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        extra = 2;
        cp = b0 & 0x0FU;
        min = 0x800;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        extra = 3;
        cp = b0 & 0x07U;
        min = 0x10000;
    } else {
        ++pos;
        return kReplacement;
    }
    if (pos + static_cast<std::size_t>(extra) >= s.size()) { // truncated sequence
        ++pos;
        return kReplacement;
    }
    for (int i = 1; i <= extra; ++i) {
        const unsigned char b = byte(pos + static_cast<std::size_t>(i));
        if ((b & 0xC0U) != 0x80U) {
            ++pos;
            return kReplacement;
        }
        cp = (cp << 6U) | (b & 0x3FU);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++pos;
        return kReplacement;
    }
    pos += static_cast<std::size_t>(extra) + 1U;
    return cp;
}

Result<std::unique_ptr<FontCache>> FontCache::create() {
    auto lib = FontLibrary::create();
    if (!lib) {
        return fail(lib.error().code, lib.error().message);
    }
    std::unique_ptr<FontCache> cache(new FontCache());
    cache->library_ = std::move(*lib);

    auto regular = find_font(preferred_families(), FontWeight::Regular);
    if (!regular) {
        return fail(regular.error().code, regular.error().message);
    }
    cache->files_[0] = *regular;
    // Heavier weights from the same family; fontconfig picks the nearest available face.
    const std::vector<std::string> family{regular->family};
    for (const FontWeight weight : {FontWeight::Medium, FontWeight::SemiBold}) {
        auto f = find_font(family, weight);
        cache->files_.at(static_cast<std::size_t>(weight)) = f ? *f : *regular;
    }
    // Phase 6: monospace for the Inspector's text view (fontconfig resolves the alias).
    auto mono = find_font({"monospace", "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono"},
                          FontWeight::Regular);
    cache->files_[3] = mono ? *mono : *regular;
    return cache;
}

const FontFile& FontCache::file_for(const TextStyle& style) const noexcept {
    return style.mono ? files_[3] : files_.at(static_cast<std::size_t>(style.weight));
}

void FontCache::clear() {
    layouts_.clear(); // layouts point at fonts: drop them first
    fonts_.clear();
}

Result<Font*> FontCache::open(const FontFile& file, int size_q) {
    for (Slot& s : fonts_) {
        if (s.file == &file && s.size_q == size_q) {
            return s.font.get();
        }
    }
    auto font = Font::open(library_, file, static_cast<float>(size_q) / 8.0F);
    if (!font) {
        return fail(font.error().code, font.error().message);
    }
    fonts_.push_back(Slot{&file, size_q, std::move(*font)});
    return fonts_.back().font.get();
}

Result<Font*> FontCache::get(const TextStyle& style, float scale) {
    const int q = quantize_size(std::max(1.0F, style.size * scale));
    return open(file_for(style), q);
}

Font* FontCache::fallback_font(char32_t codepoint, FontWeight weight, int size_q) {
    const auto w = static_cast<std::size_t>(weight);
    if (!fallback_tried_.at(w)) {
        fallback_tried_.at(w) = true; // one FcFontSort per weight, on the first miss only
        if (auto fb = FontFallback::create(weight)) {
            fallbacks_.at(w) = std::move(*fb);
        }
    }
    FontFallback* fb = fallbacks_.at(w).get();
    if (fb == nullptr) {
        return nullptr;
    }
    const FontFile* file = fb->find(codepoint);
    if (file == nullptr) {
        return nullptr;
    }
    auto font = open(*file, size_q);
    // fontconfig's charset can disagree with the cmap (rare); trust the face.
    return font && (*font)->has_glyph(codepoint) ? *font : nullptr;
}

TextLayout FontCache::layout(const TextStyle& style, Font& primary, int size_q,
                             std::string_view utf8, std::string_view language) {
    struct Item {
        Font* font;
        std::size_t begin;
        std::size_t end;
    };
    std::vector<Item> items;
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const std::size_t start = pos;
        const char32_t cp = decode_utf8(utf8, pos);
        Font* current = items.empty() ? nullptr : items.back().font;
        // Keep the current run for marks and spaces, and for anything the UI face lacks but the
        // current (fallback) face has, so a CJK phrase stays one run.
        const bool keep =
            current != nullptr &&
            (attaches_to_previous(cp) || (is_space(cp) && current->has_glyph(cp)) ||
             (current != &primary && !primary.has_glyph(cp) && current->has_glyph(cp)));
        Font* font = nullptr;
        if (keep) {
            font = current;
        } else if (primary.has_glyph(cp)) {
            font = &primary;
        } else {
            font = fallback_font(cp, style.weight, size_q);
            if (font == nullptr) {
                font = &primary; // nothing covers it: .notdef from the UI face
            }
        }
        if (font == current) {
            items.back().end = pos;
        } else {
            items.push_back(Item{font, start, pos});
        }
    }

    TextLayout out;
    out.cap_height = primary.metrics().cap_height;
    ShapeOptions options;
    options.tracking_em = style.tracking;
    options.tabular_figures = style.tabular;
    options.language = language;
    const float tracking = style.tracking * primary.pixel_size();
    float pen = 0.0F;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        TextLayout::Run run;
        run.font = it.font;
        run.shaped = it.font->shape(utf8, it.begin, it.end - it.begin, options);
        run.x = pen;
        pen += run.shaped.advance;
        if (i + 1 < items.size()) {
            pen += tracking;
        }
        out.runs.push_back(std::move(run));
    }
    out.advance = pen;
    return out;
}

std::shared_ptr<const TextLayout> FontCache::shape(const TextStyle& style, float scale,
                                                   std::string_view utf8,
                                                   std::string_view language) {
    const int q = quantize_size(std::max(1.0F, style.size * scale));
    key_.clear();
    const auto weight =
        static_cast<std::uint8_t>(static_cast<unsigned>(style.weight) | (style.mono ? 0x80U : 0U));
    const auto tracking = std::bit_cast<std::uint32_t>(style.tracking);
    const std::uint8_t tabular = style.tabular ? 1U : 0U;
    append_bytes(key_, &weight, sizeof weight);
    append_bytes(key_, &q, sizeof q);
    append_bytes(key_, &tracking, sizeof tracking);
    append_bytes(key_, &tabular, sizeof tabular);
    key_.append(language);
    key_.push_back('\0');
    key_.append(utf8);
    if (auto hit = layouts_.find(key_)) {
        return hit;
    }
    auto primary = open(file_for(style), q);
    if (!primary) {
        return std::make_shared<const TextLayout>();
    }
    return layouts_.insert(key_, layout(style, **primary, q, utf8, language));
}

} // namespace navigator::ui::text
