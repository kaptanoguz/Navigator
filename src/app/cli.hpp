// src/app/cli.hpp
//
// Command-line parsing and UI language detection for the navigator executable. Pure functions
// (no SDL, no environment access) so tests can cover them.
// Thread-safety: pure functions.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/i18n.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

namespace navigator::app {

struct CliOptions {
    // config.animations follows --no-animations. config.start_path is left empty: it is a view,
    // and CliOptions is moved around; main() points it at `start_path` right before
    // App::create().
    AppConfig config;
    std::string start_path; // positional PATH (raw bytes); empty: home
    bool show_help = false;
    bool show_version = false;
    std::optional<Language> language; // --lang overrides the environment
    // Development-only (hidden from --help): passed to the UI through NAVIGATOR_DEBUG_* env vars.
    std::string screenshot_path;
    std::optional<float> scale;
    std::string debug_script; // --debug-script STEPS (see src/ui/shell/app.cpp)
    int bench_scroll = 0;     // --bench-scroll N: generated N-entry fixture, scroll benchmark
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
