// src/ui/text/font.cpp — fontconfig discovery, FreeType faces, HarfBuzz shaping. See font.hpp.
#include "text/font.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include <hb-ft.h>
#include <hb.h>

namespace navigator::ui::text {

// ---------------------------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------------------------
namespace {

struct FcPatternDeleter {
    void operator()(FcPattern* p) const noexcept { FcPatternDestroy(p); }
};
using FcPatternPtr = std::unique_ptr<FcPattern, FcPatternDeleter>;

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

int fc_weight(FontWeight weight) noexcept {
    switch (weight) {
    case FontWeight::Regular: return FC_WEIGHT_REGULAR;
    case FontWeight::Medium: return FC_WEIGHT_MEDIUM;
    case FontWeight::SemiBold: return FC_WEIGHT_DEMIBOLD;
    }
    return FC_WEIGHT_REGULAR;
}

// Turkish letters the UI must render: İ ı Ğ ğ Ş ş Ç ç Ö ö Ü ü.
constexpr std::array<FcChar32, 12> kTurkish{0x130, 0x131, 0x11E, 0x11F, 0x15E, 0x15F,
                                            0xC7,  0xE7,  0xD6,  0xF6,  0xDC,  0xFC};

bool covers_turkish(FcPattern* match) {
    FcCharSet* cs = nullptr;
    if (FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) != FcResultMatch || cs == nullptr) {
        return false;
    }
    return std::ranges::all_of(kTurkish, [cs](FcChar32 c) { return FcCharSetHasChar(cs, c); });
}

bool has_family(FcPattern* match, std::string_view family) {
    FcChar8* value = nullptr;
    for (int i = 0; FcPatternGetString(match, FC_FAMILY, i, &value) == FcResultMatch; ++i) {
        if (iequals(reinterpret_cast<const char*>(value), family)) {
            return true;
        }
    }
    return false;
}

// Initializes fontconfig once and releases its configuration at process exit (FcFini), so leak
// checkers stay quiet. Every FcPattern we create is destroyed before then.
bool fontconfig_ready() {
    struct Session {
        bool ok = FcInit() == FcTrue;
        Session() = default;
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;
        ~Session() {
            if (ok) {
                FcFini();
            }
        }
    };
    static const Session session;
    return session.ok;
}

} // namespace

std::vector<std::string> preferred_families() {
    std::vector<std::string> out;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once at startup on the UI thread.
    if (const char* env = std::getenv("NAVIGATOR_FONT_FAMILY"); env != nullptr && *env != '\0') {
        out.emplace_back(env);
    }
    for (const char* f : {"Inter", "Cantarell", "Ubuntu Sans", "Noto Sans", "sans-serif"}) {
        out.emplace_back(f);
    }
    return out;
}

Result<FontFile> find_font(const std::vector<std::string>& families, FontWeight weight) {
    if (!fontconfig_ready()) {
        return fail(ErrorCode::PlatformError, "fontconfig initialization failed");
    }
    for (const std::string& family : families) {
        const bool generic = family == "sans-serif";
        FcPatternPtr pattern{FcPatternCreate()};
        if (!pattern) {
            return fail(ErrorCode::OutOfMemory, "FcPatternCreate failed");
        }
        FcPatternAddString(pattern.get(), FC_FAMILY,
                           reinterpret_cast<const FcChar8*>(family.c_str()));
        FcPatternAddInteger(pattern.get(), FC_WEIGHT, fc_weight(weight));
        FcPatternAddInteger(pattern.get(), FC_SLANT, FC_SLANT_ROMAN);
        FcPatternAddBool(pattern.get(), FC_SCALABLE, FcTrue);
        FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
        FcDefaultSubstitute(pattern.get());

        FcResult result = FcResultNoMatch;
        FcPatternPtr match{FcFontMatch(nullptr, pattern.get(), &result)};
        if (!match || result != FcResultMatch) {
            continue;
        }
        if (!generic && !has_family(match.get(), family)) {
            continue; // fontconfig substituted another family; try the next preference
        }
        if (!covers_turkish(match.get())) {
            continue;
        }
        FcChar8* file = nullptr;
        if (FcPatternGetString(match.get(), FC_FILE, 0, &file) != FcResultMatch) {
            continue;
        }
        FontFile out;
        out.path = reinterpret_cast<const char*>(file);
        int index = 0;
        if (FcPatternGetInteger(match.get(), FC_INDEX, 0, &index) == FcResultMatch) {
            out.index = index;
        }
        FcChar8* real_family = nullptr;
        if (FcPatternGetString(match.get(), FC_FAMILY, 0, &real_family) == FcResultMatch) {
            out.family = reinterpret_cast<const char*>(real_family);
        }
        return out;
    }
    std::string tried;
    for (const std::string& f : families) {
        tried += tried.empty() ? f : ", " + f;
    }
    return fail(ErrorCode::NotFound,
                "no scalable UI font with Turkish coverage found via fontconfig (tried: " + tried +
                    "); install fonts-noto-core or fonts-cantarell");
}

// ---------------------------------------------------------------------------------------------
// FontFallback
// ---------------------------------------------------------------------------------------------
struct FontFallback::Impl {
    struct Candidate {
        FontFile file;
        FcCharSet* charset = nullptr; // owned reference (FcCharSetCopy)
    };
    std::vector<Candidate> candidates;
    std::unordered_map<char32_t, int> memo; // codepoint -> candidate index, -1 = none

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        for (Candidate& c : candidates) {
            if (c.charset != nullptr) {
                FcCharSetDestroy(c.charset);
            }
        }
    }
};

FontFallback::FontFallback(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FontFallback::~FontFallback() = default;

Result<std::unique_ptr<FontFallback>> FontFallback::create(FontWeight weight) {
    if (!fontconfig_ready()) {
        return fail(ErrorCode::PlatformError, "fontconfig initialization failed");
    }
    FcPatternPtr pattern{FcPatternCreate()};
    if (!pattern) {
        return fail(ErrorCode::OutOfMemory, "FcPatternCreate failed");
    }
    FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>("sans-serif"));
    FcPatternAddInteger(pattern.get(), FC_WEIGHT, fc_weight(weight));
    FcPatternAddInteger(pattern.get(), FC_SLANT, FC_SLANT_ROMAN);
    FcPatternAddBool(pattern.get(), FC_SCALABLE, FcTrue);
    FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
    FcDefaultSubstitute(pattern.get());

    FcResult result = FcResultNoMatch;
    FcFontSet* set = FcFontSort(nullptr, pattern.get(), FcTrue, nullptr, &result);
    auto impl = std::make_unique<Impl>();
    if (set != nullptr) {
        impl->candidates.reserve(static_cast<std::size_t>(set->nfont));
        for (int i = 0; i < set->nfont; ++i) {
            FcPattern* font = set->fonts[i];
            FcBool scalable = FcFalse;
            FcBool outline = FcFalse;
            if (FcPatternGetBool(font, FC_SCALABLE, 0, &scalable) != FcResultMatch ||
                scalable == FcFalse ||
                FcPatternGetBool(font, FC_OUTLINE, 0, &outline) != FcResultMatch ||
                outline == FcFalse) {
                continue;
            }
#ifdef FC_COLOR
            FcBool color = FcFalse;
            if (FcPatternGetBool(font, FC_COLOR, 0, &color) == FcResultMatch && color != FcFalse) {
                continue; // colour bitmap/COLR fonts: not supported by the coverage atlas
            }
#endif
            FcChar8* path = nullptr;
            FcCharSet* cs = nullptr;
            if (FcPatternGetString(font, FC_FILE, 0, &path) != FcResultMatch ||
                FcPatternGetCharSet(font, FC_CHARSET, 0, &cs) != FcResultMatch || cs == nullptr) {
                continue;
            }
            Impl::Candidate c;
            c.file.path = reinterpret_cast<const char*>(path);
            int index = 0;
            if (FcPatternGetInteger(font, FC_INDEX, 0, &index) == FcResultMatch) {
                c.file.index = index;
            }
            FcChar8* family = nullptr;
            if (FcPatternGetString(font, FC_FAMILY, 0, &family) == FcResultMatch) {
                c.file.family = reinterpret_cast<const char*>(family);
            }
            c.charset = FcCharSetCopy(cs);
            impl->candidates.push_back(std::move(c));
        }
        FcFontSetDestroy(set);
    }
    return std::unique_ptr<FontFallback>(new FontFallback(std::move(impl)));
}

const FontFile* FontFallback::find(char32_t codepoint) {
    Impl& s = *impl_;
    int found = -1;
    if (const auto it = s.memo.find(codepoint); it != s.memo.end()) {
        found = it->second;
    } else {
        for (std::size_t i = 0; i < s.candidates.size(); ++i) {
            if (FcCharSetHasChar(s.candidates[i].charset, static_cast<FcChar32>(codepoint)) !=
                FcFalse) {
                found = static_cast<int>(i);
                break;
            }
        }
        s.memo.emplace(codepoint, found);
    }
    return found >= 0 ? &s.candidates[static_cast<std::size_t>(found)].file : nullptr;
}

std::size_t FontFallback::candidate_count() const noexcept { return impl_->candidates.size(); }

// ---------------------------------------------------------------------------------------------
// FontLibrary
// ---------------------------------------------------------------------------------------------
struct FontLibrary::Impl {
    FT_Library library = nullptr;
    ~Impl() {
        if (library != nullptr) {
            FT_Done_FreeType(library);
        }
    }
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};

FontLibrary::FontLibrary(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FontLibrary::~FontLibrary() = default;

Result<std::shared_ptr<FontLibrary>> FontLibrary::create() {
    auto impl = std::make_unique<Impl>();
    if (FT_Init_FreeType(&impl->library) != 0) {
        impl->library = nullptr;
        return fail(ErrorCode::PlatformError, "FT_Init_FreeType failed");
    }
    return std::shared_ptr<FontLibrary>(new FontLibrary(std::move(impl)));
}

// ---------------------------------------------------------------------------------------------
// Font
// ---------------------------------------------------------------------------------------------
namespace {

constexpr float k26_6 = 64.0F;

float from_26_6(FT_Pos v) noexcept { return static_cast<float>(v) / k26_6; }

std::atomic<std::uint32_t> g_next_font_id{1};

} // namespace

struct Font::Impl {
    std::shared_ptr<FontLibrary> library;
    FontFile file;
    FT_Face face = nullptr;
    hb_font_t* hb_font = nullptr;
    hb_buffer_t* buffer = nullptr;
    float pixel_size = 0.0F;
    FontMetrics metrics;
    std::uint32_t id = 0;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        if (buffer != nullptr) {
            hb_buffer_destroy(buffer);
        }
        if (hb_font != nullptr) {
            hb_font_destroy(hb_font);
        }
        if (face != nullptr) {
            FT_Done_Face(face);
        }
    }
};

Font::Font(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Font::~Font() = default;

Result<std::unique_ptr<Font>> Font::open(std::shared_ptr<FontLibrary> library, const FontFile& file,
                                         float pixel_size) {
    if (!library) {
        return fail(ErrorCode::InvalidArgument, "null FontLibrary");
    }
    if (!std::isfinite(pixel_size) || pixel_size < 1.0F || pixel_size > 1024.0F) {
        return fail(ErrorCode::InvalidArgument, "font pixel size out of range");
    }
    auto impl = std::make_unique<Impl>();
    impl->library = std::move(library);
    impl->file = file;
    impl->pixel_size = pixel_size;
    if (FT_New_Face(impl->library->impl().library, file.path.c_str(), file.index, &impl->face) !=
        0) {
        impl->face = nullptr;
        return fail(ErrorCode::IoError, "cannot open font face: " + file.path);
    }
    const auto size_26_6 = static_cast<FT_F26Dot6>(std::lround(pixel_size * k26_6));
    if (FT_Set_Char_Size(impl->face, 0, size_26_6, 72, 72) != 0) {
        return fail(ErrorCode::InvalidArgument, "font is not scalable: " + file.path);
    }

    const FT_Size_Metrics& m = impl->face->size->metrics;
    impl->metrics.ascender = from_26_6(m.ascender);
    impl->metrics.descender = -from_26_6(m.descender);
    impl->metrics.line_height = from_26_6(m.height);
    const auto font_units = [&](FT_Short v) {
        return static_cast<float>(v) * pixel_size / static_cast<float>(impl->face->units_per_EM);
    };
    const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(impl->face, FT_SFNT_OS2));
    if (os2 != nullptr && os2->version >= 2 && os2->sCapHeight > 0) {
        impl->metrics.cap_height = font_units(os2->sCapHeight);
        impl->metrics.x_height = font_units(os2->sxHeight);
    } else {
        impl->metrics.cap_height = impl->metrics.ascender * 0.7F;
        impl->metrics.x_height = impl->metrics.ascender * 0.5F;
    }

    impl->hb_font = hb_ft_font_create_referenced(impl->face);
    if (impl->hb_font == nullptr) {
        return fail(ErrorCode::OutOfMemory, "hb_ft_font_create failed");
    }
    // Unhinted advances: exact positions for subpixel placement.
    hb_ft_font_set_load_flags(impl->hb_font, FT_LOAD_NO_HINTING);
    impl->buffer = hb_buffer_create();
    if (hb_buffer_allocation_successful(impl->buffer) == 0) {
        return fail(ErrorCode::OutOfMemory, "hb_buffer_create failed");
    }
    impl->id = g_next_font_id.fetch_add(1, std::memory_order_relaxed);
    return std::unique_ptr<Font>(new Font(std::move(impl)));
}

std::uint32_t Font::id() const noexcept { return impl_->id; }
float Font::pixel_size() const noexcept { return impl_->pixel_size; }
const FontMetrics& Font::metrics() const noexcept { return impl_->metrics; }
const FontFile& Font::file() const noexcept { return impl_->file; }

bool Font::has_glyph(char32_t codepoint) const noexcept {
    return FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>(codepoint)) != 0;
}

ShapedRun Font::shape(std::string_view utf8, const ShapeOptions& options) {
    return shape(utf8, 0, utf8.size(), options);
}

ShapedRun Font::shape(std::string_view utf8, std::size_t offset, std::size_t length,
                      const ShapeOptions& options) {
    ShapedRun run;
    if (offset >= utf8.size() || length == 0) {
        return run;
    }
    length = std::min(length, utf8.size() - offset);
    hb_buffer_t* buf = impl_->buffer;
    hb_buffer_clear_contents(buf);
    hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()),
                       static_cast<unsigned int>(offset), static_cast<int>(length));
    hb_buffer_set_language(buf, hb_language_from_string(options.language.data(),
                                                        static_cast<int>(options.language.size())));
    hb_buffer_guess_segment_properties(buf);

    std::array<hb_feature_t, 1> features{};
    unsigned int feature_count = 0;
    if (options.tabular_figures) {
        features[0] = hb_feature_t{HB_TAG('t', 'n', 'u', 'm'), 1, HB_FEATURE_GLOBAL_START,
                                   HB_FEATURE_GLOBAL_END};
        feature_count = 1;
    }
    hb_shape(impl_->hb_font, buf, features.data(), feature_count);

    unsigned int count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
    run.glyphs.reserve(count);
    const float tracking = options.tracking_em * impl_->pixel_size;
    float pen = 0.0F;
    for (unsigned int i = 0; i < count; ++i) {
        ShapedGlyph g;
        g.glyph = info[i].codepoint;
        g.cluster = info[i].cluster;
        g.x = pen + from_26_6(pos[i].x_offset);
        g.y = -from_26_6(pos[i].y_offset);
        run.glyphs.push_back(g);
        pen += from_26_6(pos[i].x_advance);
        const bool cluster_ends = i + 1 == count || info[i + 1].cluster != info[i].cluster;
        if (cluster_ends && i + 1 != count) {
            pen += tracking;
        }
    }
    run.advance = pen;
    return run;
}

Result<GlyphBitmap> Font::rasterize(std::uint32_t glyph, float subpixel_x) {
    FT_Face face = impl_->face;
    if (FT_Load_Glyph(face, glyph, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) != 0) {
        return fail(ErrorCode::NotFound, "FT_Load_Glyph failed");
    }
    FT_GlyphSlot slot = face->glyph;
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_Outline_Translate(&slot->outline, static_cast<FT_Pos>(std::lround(subpixel_x * k26_6)),
                             0);
    }
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT) != 0) {
        return fail(ErrorCode::Unsupported, "FT_Render_Glyph failed");
    }
    const FT_Bitmap& bm = slot->bitmap;
    GlyphBitmap out;
    out.width = static_cast<int>(bm.width);
    out.height = static_cast<int>(bm.rows);
    out.left = slot->bitmap_left;
    out.top = slot->bitmap_top;
    out.coverage.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
    const int pitch = bm.pitch;
    for (int y = 0; y < out.height; ++y) {
        const unsigned char* row =
            pitch >= 0 ? bm.buffer + (static_cast<std::ptrdiff_t>(y) * pitch)
                       : bm.buffer + (static_cast<std::ptrdiff_t>(out.height - 1 - y) * -pitch);
        auto* dst = out.coverage.data() +
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width));
        if (bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
            std::memcpy(dst, row, static_cast<std::size_t>(out.width));
        } else if (bm.pixel_mode == FT_PIXEL_MODE_MONO) {
            for (int x = 0; x < out.width; ++x) {
                const auto bit =
                    static_cast<unsigned>(row[x >> 3]) >> (7U - (static_cast<unsigned>(x) & 7U));
                dst[x] = (bit & 1U) != 0U ? 255 : 0;
            }
        } else {
            return fail(ErrorCode::Unsupported, "unsupported glyph pixel mode");
        }
    }
    return out;
}

} // namespace navigator::ui::text
