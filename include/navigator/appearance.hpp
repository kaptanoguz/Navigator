// navigator/appearance.hpp
//
// Purpose: Phase 9B appearance settings (theme selection, custom JSON color schemes, font scale,
//          spacing/density, corner rounding, animations toggle, alternating row colors) and
//          lenient JSON (de)serialization + atomic persistence (`settings.json`, `themes/*.json`).
//          No SDL, no renderer, no JSON library types.
// Owner:   contracts (header); implementation in src/core (role: core-fs), tests in tests/core.
// Thread-safety: All types are plain values; JSON helpers are pure (callable from any thread).
//                load_* / save_* / list_custom_themes perform blocking file I/O (workers; startup
//                load and quit save are the documented UI-thread exceptions).
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>
#include <navigator/ui.hpp>

namespace navigator::appearance {

// ---------------------------------------------------------------------------------------------
// Color value & custom theme scheme (JSON)
// ---------------------------------------------------------------------------------------------

// sRGB 8-bit RGBA color (0..255 per channel).
struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend constexpr bool operator==(Rgba8, Rgba8) = default;
};

// Parses "#RGB", "#RRGGBB" or "#RRGGBBAA" (case-insensitive). Returns nullopt on invalid input.
[[nodiscard]] std::optional<Rgba8> parse_hex_color(std::string_view text) noexcept;

// Formats as "#RRGGBB" (when a == 255) or "#RRGGBBAA".
[[nodiscard]] std::string format_hex_color(Rgba8 color);

// Semantic color overrides in a custom JSON color scheme. Any omitted field falls back to `base`
// (Theme::Dark or Theme::Light) when resolved into a UI Palette.
struct ThemeColors {
    std::optional<Rgba8> background;
    std::optional<Rgba8> background_alt;
    std::optional<Rgba8> surface;
    std::optional<Rgba8> surface_raised;
    std::optional<Rgba8> text_primary;
    std::optional<Rgba8> text_secondary;
    std::optional<Rgba8> text_muted;
    std::optional<Rgba8> accent;
    std::optional<Rgba8> on_accent;
    std::optional<Rgba8> border;
    std::optional<Rgba8> danger;
    std::optional<Rgba8> warning;
    std::optional<Rgba8> selection;
    std::optional<Rgba8> row_alt;
    std::optional<Rgba8> row_hover;
    std::optional<Rgba8> focus_ring;
    std::optional<Rgba8> icon_folder_back;
    std::optional<Rgba8> icon_folder_front;

    friend bool operator==(const ThemeColors&, const ThemeColors&) = default;
};

struct ThemeScheme {
    static constexpr int kVersion = 1;

    std::string id;   // unique identifier (e.g. "dark", "light", or filename stem "nord")
    std::string name; // display name shown in Settings
    Theme base = Theme::Dark;
    ThemeColors colors;

    friend bool operator==(const ThemeScheme&, const ThemeScheme&) = default;
};

inline constexpr std::string_view kBuiltinDarkId = "dark";
inline constexpr std::string_view kBuiltinLightId = "light";

// ---------------------------------------------------------------------------------------------
// Appearance settings (persisted in <config_root>/settings.json)
// ---------------------------------------------------------------------------------------------

enum class UiDensity : std::uint8_t {
    Compact = 0,
    Comfortable = 1,
};

inline constexpr int kMinFontScalePercent = 75;
inline constexpr int kMaxFontScalePercent = 150;
inline constexpr int kDefaultFontScalePercent = 100;
inline constexpr int kFontScaleStepPercent = 5;

inline constexpr int kMinSpacingScalePercent = 75;
inline constexpr int kMaxSpacingScalePercent = 150;
inline constexpr int kDefaultSpacingScalePercent = 100;
inline constexpr int kSpacingScaleStepPercent = 5;

inline constexpr int kMinCornerRadiusDp = 0;
inline constexpr int kMaxCornerRadiusDp = 14;
inline constexpr int kDefaultCornerRadiusDp = 6;

[[nodiscard]] constexpr int clamp_font_scale(int percent) noexcept {
    if (percent < kMinFontScalePercent) {
        return kMinFontScalePercent;
    }
    if (percent > kMaxFontScalePercent) {
        return kMaxFontScalePercent;
    }
    return percent;
}

[[nodiscard]] constexpr int clamp_spacing_scale(int percent) noexcept {
    if (percent < kMinSpacingScalePercent) {
        return kMinSpacingScalePercent;
    }
    if (percent > kMaxSpacingScalePercent) {
        return kMaxSpacingScalePercent;
    }
    return percent;
}

[[nodiscard]] constexpr int clamp_corner_radius(int dp) noexcept {
    if (dp < kMinCornerRadiusDp) {
        return kMinCornerRadiusDp;
    }
    if (dp > kMaxCornerRadiusDp) {
        return kMaxCornerRadiusDp;
    }
    return dp;
}

[[nodiscard]] std::string_view to_string(UiDensity density) noexcept;
[[nodiscard]] std::optional<UiDensity> parse_density(std::string_view name) noexcept;

struct AppearanceSettings {
    static constexpr int kVersion = 1;

    std::string theme_id = std::string(kBuiltinDarkId); // "dark", "light", or custom scheme id
    UiDensity density = UiDensity::Compact;
    int font_scale_percent = kDefaultFontScalePercent;
    int spacing_scale_percent = kDefaultSpacingScalePercent;
    int corner_radius_dp = kDefaultCornerRadiusDp;
    bool animations = true;
    bool alternating_rows = true;

    friend bool operator==(const AppearanceSettings&, const AppearanceSettings&) = default;
};

[[nodiscard]] inline AppearanceSettings normalize(AppearanceSettings s) {
    if (s.theme_id.empty()) {
        s.theme_id = std::string(kBuiltinDarkId);
    }
    s.font_scale_percent = clamp_font_scale(s.font_scale_percent);
    s.spacing_scale_percent = clamp_spacing_scale(s.spacing_scale_percent);
    s.corner_radius_dp = clamp_corner_radius(s.corner_radius_dp);
    return s;
}

// ---------------------------------------------------------------------------------------------
// JSON and files
// ---------------------------------------------------------------------------------------------

inline constexpr std::string_view kSettingsFileName = "settings.json";
inline constexpr std::string_view kThemesDirName = "themes";

[[nodiscard]] inline Path settings_file(const Path& config_root) {
    return config_root / kSettingsFileName;
}

[[nodiscard]] inline Path themes_dir(const Path& config_root) {
    return config_root / kThemesDirName;
}

// Lenient JSON serialization/deserialization for AppearanceSettings (unknown fields ignored).
[[nodiscard]] Result<std::string> to_json(const AppearanceSettings& settings);
[[nodiscard]] Result<AppearanceSettings> appearance_from_json(std::string_view text);

// Atomic file I/O (dir 0700, file 0600, temp + fsync + rename).
[[nodiscard]] Result<AppearanceSettings> load_appearance_settings(const Path& file);
Result<void> save_appearance_settings(const Path& file, const AppearanceSettings& settings);

// Lenient JSON serialization/deserialization for custom ThemeScheme.
[[nodiscard]] Result<std::string> to_json(const ThemeScheme& scheme);
[[nodiscard]] Result<ThemeScheme> theme_scheme_from_json(std::string_view text,
                                                         std::string_view fallback_id = {});

// Loads a single custom theme JSON file (fallback_id derived from filename stem).
[[nodiscard]] Result<ThemeScheme> load_theme_scheme(const Path& file);

// Scans `dir` for `*.json` custom theme schemes (sorted by id). Missing directory returns empty.
// Invalid files are skipped without failing the scan.
[[nodiscard]] std::vector<ThemeScheme> list_custom_themes(const Path& dir);

} // namespace navigator::appearance
