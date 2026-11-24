// src/app/cli.hpp
//
// Command-line parsing and UI language detection for the navigator executable. Pure functions
// (no SDL, no environment access) so tests can cover them.
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/i18n.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

namespace navigator::app {

// List density (--density). Maps to the ui-internal ui::Density in main(); AppConfig has no
// field for it yet.
enum class DensityChoice : std::uint8_t { Compact, Comfortable };

struct CliOptions {
    // config.animations follows --no-animations; --no-session clears restore_session and
    // save_session; --size clears restore_window_geometry. config.start_path and
    // config.config_dir are left empty: they are views,
    // and CliOptions is moved around; main() points it at `start_path` right before
    // App::create().
    AppConfig config;
    std::string start_path; // positional PATH (raw bytes); empty: home
    // --config-dir DIR (raw bytes); empty: $XDG_CONFIG_HOME/navigator. main() points
    // config.config_dir at it, like start_path.
    std::string config_dir;
    // Phase 5: --cache-dir DIR (raw bytes); empty: $XDG_CACHE_HOME/navigator. main() points
    // config.cache_dir at it. --no-index clears config.enable_index.
    std::string cache_dir;
    // Phase 6: --thumbnail-dir DIR (raw bytes); empty: <cache_dir>/thumbnails, else the shared
    // Freedesktop cache. main() points config.thumbnail_dir at it. --no-thumbnails clears
    // config.enable_thumbnails, --no-external-thumbnailers config.enable_external_thumbnailers,
    // --preview-cache-mb N sets config.preview_cache_bytes.
    std::string thumbnail_dir;
    // Phase 7: --trash-dir DIR (raw bytes); empty: the real XDG trash (never in a --frames run,
    // see AppConfig::trash_dir). main() points config.trash_dir at it.
    std::string trash_dir;
    // Phase 10: single-instance IPC, --new-tab, --select, plugins, git, gvfs.
    std::string ipc_socket;
    std::string plugin_dir;
    std::string select_path;
    bool new_tab = false;
    bool new_instance = false;
    bool show_help = false;
    bool show_version = false;
    std::optional<Language> language;               // --lang overrides the environment
    DensityChoice density = DensityChoice::Compact; // --density compact|comfortable
    // Development-only (hidden from --help): passed to the UI through NAVIGATOR_DEBUG_* env vars.
    std::string screenshot_path;
    std::optional<float> scale;
    std::string debug_script; // --debug-script STEPS (see src/ui/shell/app.cpp)
    int bench_scroll = 0;     // --bench-scroll N: generated N-entry fixture, scroll benchmark
    int bench_flatten = 0;    // --bench-flatten N: generated N-entry tree, flatten + cancel
};

// `args` excludes argv[0]. Errors: InvalidArgument with a user-facing English message.
[[nodiscard]] Result<CliOptions> parse_cli(const std::vector<std::string_view>& args);

// UI language from POSIX locale variables, in precedence order LANGUAGE (first entry), LC_ALL,
// LC_MESSAGES, LANG. A value starting with "tr" (tr, tr_TR.UTF-8, tr@...) selects Turkish.
[[nodiscard]] Language detect_language(std::string_view language_var, std::string_view lc_all,
                                       std::string_view lc_messages, std::string_view lang);

[[nodiscard]] std::string usage_text(std::string_view program);
[[nodiscard]] std::string_view version_string() noexcept;

} // namespace navigator::app
