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
    AppConfig config;
    bool show_help = false;
    bool show_version = false;
    std::optional<Language> language; // --lang overrides the environment
    bool no_animations = false;       // --no-animations (reaches the UI as NAVIGATOR_NO_ANIMATIONS)
    // Development-only (hidden from --help): passed to the UI through NAVIGATOR_DEBUG_* env vars.
    std::string screenshot_path;
    std::optional<float> scale;
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
