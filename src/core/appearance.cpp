// src/core/appearance.cpp — Phase 9B appearance settings and custom JSON theme schemes.
#include <navigator/appearance.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "document_files.hpp"
#include "json_util.hpp"

namespace navigator::appearance {
namespace {

using core::json::as_bool;
using core::json::as_integer;
using core::json::as_string;
using core::json::dump;
using core::json::InJson;
using core::json::member;
using core::json::OutJson;
using core::json::parse_object;

[[nodiscard]] int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

[[nodiscard]] std::optional<std::uint8_t> hex_byte(char hi, char lo) noexcept {
    const int h = hex_nibble(hi);
    const int l = hex_nibble(lo);
    if (h < 0 || l < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>((h << 4) | l);
}

void write_opt_color(OutJson& obj, const char* key, const std::optional<Rgba8>& c) {
    if (c.has_value()) {
        obj[key] = format_hex_color(*c);
    }
}

void read_opt_color(const InJson& obj, const char* key, std::optional<Rgba8>& out) {
    if (const auto s = as_string(member(obj, key))) {
        if (const auto c = parse_hex_color(*s)) {
            out = *c;
        }
    }
}

} // namespace

std::optional<Rgba8> parse_hex_color(std::string_view text) noexcept {
    if (text.empty() || text.front() != '#') {
        return std::nullopt;
    }
    text.remove_prefix(1);
    if (text.size() == 3) {
        const auto r = hex_byte(text[0], text[0]);
        const auto g = hex_byte(text[1], text[1]);
        const auto b = hex_byte(text[2], text[2]);
        if (!r || !g || !b) {
            return std::nullopt;
        }
        return Rgba8{*r, *g, *b, 255};
    }
    if (text.size() == 6 || text.size() == 8) {
        const auto r = hex_byte(text[0], text[1]);
        const auto g = hex_byte(text[2], text[3]);
        const auto b = hex_byte(text[4], text[5]);
        if (!r || !g || !b) {
            return std::nullopt;
        }
        std::uint8_t a = 255;
        if (text.size() == 8) {
            const auto alpha = hex_byte(text[6], text[7]);
            if (!alpha) {
                return std::nullopt;
            }
            a = *alpha;
        }
        return Rgba8{*r, *g, *b, a};
    }
    return std::nullopt;
}

std::string format_hex_color(Rgba8 c) {
    std::array<char, 16> buf{};
    if (c.a == 255) {
        std::snprintf(buf.data(), buf.size(), "#%02X%02X%02X", static_cast<unsigned>(c.r),
                      static_cast<unsigned>(c.g), static_cast<unsigned>(c.b));
    } else {
        std::snprintf(buf.data(), buf.size(), "#%02X%02X%02X%02X", static_cast<unsigned>(c.r),
                      static_cast<unsigned>(c.g), static_cast<unsigned>(c.b),
                      static_cast<unsigned>(c.a));
    }
    return std::string(buf.data());
}

std::string_view to_string(UiDensity density) noexcept {
    switch (density) {
    case UiDensity::Compact: return "compact";
    case UiDensity::Comfortable: return "comfortable";
    }
    return "compact";
}

std::optional<UiDensity> parse_density(std::string_view name) noexcept {
    if (name == "compact") {
        return UiDensity::Compact;
    }
    if (name == "comfortable") {
        return UiDensity::Comfortable;
    }
    return std::nullopt;
}

Result<std::string> to_json(const AppearanceSettings& raw) {
    try {
        const AppearanceSettings s = normalize(raw);
        OutJson root = OutJson::object();
        root["version"] = AppearanceSettings::kVersion;
        root["theme"] = s.theme_id;
        root["density"] = std::string(to_string(s.density));
        root["font_scale"] = s.font_scale_percent;
        root["spacing_scale"] = s.spacing_scale_percent;
        root["corner_radius"] = s.corner_radius_dp;
        root["animations"] = s.animations;
        root["alternating_rows"] = s.alternating_rows;
        return dump(root);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory serializing appearance settings");
    }
}

Result<AppearanceSettings> appearance_from_json(std::string_view text) {
    try {
        auto parsed = parse_object(text);
        if (!parsed) {
            return Unexpected<Error>(parsed.error());
        }
        const InJson& root = *parsed;
        AppearanceSettings s;
        if (const auto t = as_string(member(root, "theme")); t && !t->empty()) {
            s.theme_id = *t;
        }
        if (const auto d = as_string(member(root, "density"))) {
            if (const auto pd = parse_density(*d)) {
                s.density = *pd;
            }
        }
        if (const auto fs = as_integer(member(root, "font_scale"))) {
            s.font_scale_percent = static_cast<int>(*fs);
        }
        if (const auto ss = as_integer(member(root, "spacing_scale"))) {
            s.spacing_scale_percent = static_cast<int>(*ss);
        }
        if (const auto cr = as_integer(member(root, "corner_radius"))) {
            s.corner_radius_dp = static_cast<int>(*cr);
        }
        s.animations = as_bool(member(root, "animations"), s.animations);
        s.alternating_rows = as_bool(member(root, "alternating_rows"), s.alternating_rows);
        return normalize(std::move(s));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory parsing appearance settings");
    }
}

Result<AppearanceSettings> load_appearance_settings(const Path& file) {
    auto raw = core::read_document(file);
    if (!raw) {
        return Unexpected<Error>(raw.error());
    }
    auto parsed = appearance_from_json(*raw);
    if (!parsed) {
        return Unexpected<Error>(core::move_aside_bad(file, parsed.error().message));
    }
    return parsed;
}

Result<void> save_appearance_settings(const Path& file, const AppearanceSettings& settings) {
    auto json = to_json(settings);
    if (!json) {
        return Unexpected<Error>(json.error());
    }
    return core::write_atomically(file, *json);
}

Result<std::string> to_json(const ThemeScheme& scheme) {
    try {
        OutJson root = OutJson::object();
        root["version"] = ThemeScheme::kVersion;
        if (!scheme.id.empty()) {
            root["id"] = scheme.id;
        }
        root["name"] = scheme.name.empty() ? scheme.id : scheme.name;
        root["base"] = scheme.base == Theme::Light ? "light" : "dark";

        OutJson colors = OutJson::object();
        const ThemeColors& c = scheme.colors;
        write_opt_color(colors, "background", c.background);
        write_opt_color(colors, "background_alt", c.background_alt);
        write_opt_color(colors, "surface", c.surface);
        write_opt_color(colors, "surface_raised", c.surface_raised);
        write_opt_color(colors, "text_primary", c.text_primary);
        write_opt_color(colors, "text_secondary", c.text_secondary);
        write_opt_color(colors, "text_muted", c.text_muted);
        write_opt_color(colors, "accent", c.accent);
        write_opt_color(colors, "on_accent", c.on_accent);
        write_opt_color(colors, "border", c.border);
        write_opt_color(colors, "danger", c.danger);
        write_opt_color(colors, "warning", c.warning);
        write_opt_color(colors, "selection", c.selection);
        write_opt_color(colors, "row_alt", c.row_alt);
        write_opt_color(colors, "row_hover", c.row_hover);
        write_opt_color(colors, "focus_ring", c.focus_ring);
        write_opt_color(colors, "icon_folder_back", c.icon_folder_back);
        write_opt_color(colors, "icon_folder_front", c.icon_folder_front);
        root["colors"] = std::move(colors);
        return dump(root);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory serializing theme scheme");
    }
}

Result<ThemeScheme> theme_scheme_from_json(std::string_view text, std::string_view fallback_id) {
    try {
        auto parsed = parse_object(text);
        if (!parsed) {
            return Unexpected<Error>(parsed.error());
        }
        const InJson& root = *parsed;
        ThemeScheme scheme;
        if (const auto id = as_string(member(root, "id")); id && !id->empty()) {
            scheme.id = *id;
        } else {
            scheme.id = std::string(fallback_id);
        }
        if (const auto name = as_string(member(root, "name")); name && !name->empty()) {
            scheme.name = *name;
        } else {
            scheme.name = scheme.id;
        }
        if (const auto base = as_string(member(root, "base"))) {
            scheme.base = (*base == "light") ? Theme::Light : Theme::Dark;
        }
        if (const InJson* colors = member(root, "colors"); colors != nullptr && colors->is_object()) {
            ThemeColors& c = scheme.colors;
            read_opt_color(*colors, "background", c.background);
            read_opt_color(*colors, "background_alt", c.background_alt);
            read_opt_color(*colors, "surface", c.surface);
            read_opt_color(*colors, "surface_raised", c.surface_raised);
            read_opt_color(*colors, "text_primary", c.text_primary);
            read_opt_color(*colors, "text_secondary", c.text_secondary);
            read_opt_color(*colors, "text_muted", c.text_muted);
            read_opt_color(*colors, "accent", c.accent);
            read_opt_color(*colors, "on_accent", c.on_accent);
            read_opt_color(*colors, "border", c.border);
            read_opt_color(*colors, "danger", c.danger);
            read_opt_color(*colors, "warning", c.warning);
            read_opt_color(*colors, "selection", c.selection);
            read_opt_color(*colors, "row_alt", c.row_alt);
            read_opt_color(*colors, "row_hover", c.row_hover);
            read_opt_color(*colors, "focus_ring", c.focus_ring);
            read_opt_color(*colors, "icon_folder_back", c.icon_folder_back);
            read_opt_color(*colors, "icon_folder_front", c.icon_folder_front);
        }
        if (scheme.id.empty()) {
            return fail(ErrorCode::InvalidArgument, "theme scheme has no id");
        }
        return scheme;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory parsing theme scheme");
    }
}

Result<ThemeScheme> load_theme_scheme(const Path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file.native(), ec)) {
        return fail(ErrorCode::NotFound, "theme file not found: " + file.native());
    }
    std::ifstream in(file.native(), std::ios::binary);
    if (!in) {
        return fail(ErrorCode::IoError, "cannot read theme file: " + file.native());
    }
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    const std::string stem = std::filesystem::path(file.native()).stem().string();
    return theme_scheme_from_json(content, stem);
}

std::vector<ThemeScheme> list_custom_themes(const Path& dir) {
    std::vector<ThemeScheme> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir.native(), ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir.native(), ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() != ".json") {
            continue;
        }
        auto loaded = load_theme_scheme(Path{entry.path().string()});
        if (loaded && loaded->id != kBuiltinDarkId && loaded->id != kBuiltinLightId) {
            out.push_back(std::move(*loaded));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const ThemeScheme& a, const ThemeScheme& b) { return a.id < b.id; });
    return out;
}

} // namespace navigator::appearance
