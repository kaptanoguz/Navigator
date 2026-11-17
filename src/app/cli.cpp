// src/app/cli.cpp — see cli.hpp.
#include "cli.hpp"

#include <charconv>
#include <cstdlib>

#ifndef NAVIGATOR_VERSION_STRING
#define NAVIGATOR_VERSION_STRING "0.0.0"
#endif

namespace navigator::app {
namespace {

bool parse_int(std::string_view s, int& out) {
    const char* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

bool parse_float(std::string_view s, float& out) {
    // std::from_chars(float) is available in libstdc++ 11+.
    const char* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

// The window's minimum size (src/ui/shell/app.cpp); smaller requests would be clamped anyway.
constexpr int kMinWidth = 720;
constexpr int kMinHeight = 480;

bool is_turkish(std::string_view v) noexcept {
    if (v.size() < 2 || v.substr(0, 2) != "tr") {
        return false;
    }
    return v.size() == 2 || v[2] == '_' || v[2] == '.' || v[2] == '@' || v[2] == ':' || v[2] == '-';
}

} // namespace

Result<CliOptions> parse_cli(const std::vector<std::string_view>& args) {
    CliOptions o;
    bool options_done = false;
    bool have_path = false;
    const auto positional = [&](std::string_view a) -> Result<void> {
        if (have_path) {
            return fail(ErrorCode::InvalidArgument,
                        "only one PATH may be given, got another: '" + std::string(a) + "'");
        }
        if (a.empty()) {
            return fail(ErrorCode::InvalidArgument, "PATH must not be empty");
        }
        have_path = true;
        o.start_path = std::string(a);
        return ok();
    };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (options_done || a == "-" || a.empty() || a.front() != '-') {
            if (auto r = positional(a); !r) {
                return fail(r.error().code, r.error().message);
            }
            continue;
        }
        if (a == "--") {
            options_done = true;
            continue;
        }
        const auto value = [&](std::string_view flag) -> Result<std::string_view> {
            if (i + 1 >= args.size()) {
                return fail(ErrorCode::InvalidArgument,
                            "option " + std::string(flag) + " requires a value");
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") {
            o.show_help = true;
        } else if (a == "-V" || a == "--version") {
            o.show_version = true;
        } else if (a == "--frames") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            int n = 0;
            if (!parse_int(*v, n) || n <= 0) {
                return fail(ErrorCode::InvalidArgument,
                            "--frames expects a positive integer, got '" + std::string(*v) + "'");
            }
            o.config.max_frames = n;
        } else if (a == "--theme") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            if (*v == "dark") {
                o.config.theme = Theme::Dark;
            } else if (*v == "light") {
                o.config.theme = Theme::Light;
            } else {
                return fail(ErrorCode::InvalidArgument,
                            "--theme expects 'dark' or 'light', got '" + std::string(*v) + "'");
            }
        } else if (a == "--lang") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            if (*v == "en") {
                o.language = Language::English;
            } else if (*v == "tr") {
                o.language = Language::Turkish;
            } else {
                return fail(ErrorCode::InvalidArgument,
                            "--lang expects 'en' or 'tr', got '" + std::string(*v) + "'");
            }
        } else if (a == "--density") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            if (*v == "compact") {
                o.density = DensityChoice::Compact;
            } else if (*v == "comfortable") {
                o.density = DensityChoice::Comfortable;
            } else {
                return fail(ErrorCode::InvalidArgument,
                            "--density expects 'compact' or 'comfortable', got '" +
                                std::string(*v) + "'");
            }
        } else if (a == "--size") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            const std::size_t x = v->find('x');
            int w = 0;
            int h = 0;
            if (x == std::string_view::npos || !parse_int(v->substr(0, x), w) ||
                !parse_int(v->substr(x + 1), h) || w < kMinWidth || h < kMinHeight || w > 16384 ||
                h > 16384) {
                return fail(ErrorCode::InvalidArgument, "--size expects WIDTHxHEIGHT of at least " +
                                                            std::to_string(kMinWidth) + "x" +
                                                            std::to_string(kMinHeight) + ", got '" +
                                                            std::string(*v) + "'");
            }
            o.config.width = w;
            o.config.height = h;
            o.config.restore_window_geometry = false; // an explicit size wins over the session
        } else if (a == "--no-session") {
            o.config.restore_session = false;
            o.config.save_session = false;
        } else if (a == "--config-dir") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            if (v->empty()) {
                return fail(ErrorCode::InvalidArgument, "--config-dir must not be empty");
            }
            o.config_dir = std::string(*v);
        } else if (a == "--fps") {
            o.config.show_fps = true;
        } else if (a == "--no-fps") {
            o.config.show_fps = false;
        } else if (a == "--no-animations") {
            o.config.animations = false;
        } else if (a == "--debug-script") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            o.debug_script = std::string(*v);
        } else if (a == "--bench-scroll") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            int n = 0;
            if (!parse_int(*v, n) || n <= 0 || n > 1000000) {
                return fail(ErrorCode::InvalidArgument,
                            "--bench-scroll expects an entry count in [1, 1000000], got '" +
                                std::string(*v) + "'");
            }
            o.bench_scroll = n;
        } else if (a == "--screenshot") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            o.screenshot_path = std::string(*v);
        } else if (a == "--scale") {
            auto v = value(a);
            if (!v) {
                return fail(v.error().code, v.error().message);
            }
            float s = 0.0F;
            if (!parse_float(*v, s) || s < 0.5F || s > 4.0F) {
                return fail(ErrorCode::InvalidArgument,
                            "--scale expects a number in [0.5, 4], got '" + std::string(*v) + "'");
            }
            o.scale = s;
        } else {
            return fail(ErrorCode::InvalidArgument, "unknown option '" + std::string(a) + "'");
        }
    }
    if (!o.screenshot_path.empty() && o.config.max_frames == 0) {
        o.config.max_frames = 3; // a screenshot run renders a few frames and exits
    }
    return o;
}

Language detect_language(std::string_view language_var, std::string_view lc_all,
                         std::string_view lc_messages, std::string_view lang) {
    // LANGUAGE is a colon-separated priority list; its first entry wins.
    if (const auto colon = language_var.find(':'); colon != std::string_view::npos) {
        language_var = language_var.substr(0, colon);
    }
    for (const std::string_view v : {language_var, lc_all, lc_messages, lang}) {
        if (!v.empty()) {
            return is_turkish(v) ? Language::Turkish : Language::English;
        }
    }
    return Language::English;
}

std::string usage_text(std::string_view program) {
    std::string s;
    s += "Usage: ";
    s += program;
    s += " [options] [PATH]\n\n"
         "Navigator, a fast keyboard-first file manager for Linux.\n\n"
         "Restores your last session (tabs, panes, window) and opens PATH as a new tab\n"
         "(default: your home folder when there is no session).\n\n"
         "Options:\n"
         "  --frames N        render N frames, then exit (headless smoke tests)\n"
         "  --theme THEME     start with the 'dark' or 'light' theme\n"
         "  --lang LANG       UI language: 'en' or 'tr' (default: from LANGUAGE/LC_*/LANG)\n"
         "  --density D       list density: 'compact' (default) or 'comfortable'\n"
         "  --size WxH        initial window size in logical pixels (default 1280x800;\n"
         "                    overrides the size saved in the session)\n"
         "  --no-session      do not restore or save the session\n"
         "  --config-dir DIR  keep session.json and layouts/ in DIR\n"
         "                    (default: $XDG_CONFIG_HOME/navigator)\n"
         "  --fps, --no-fps   show or hide the performance overlay (default: hidden)\n"
         "  --no-animations   turn off UI animations such as smooth scrolling\n"
         "                    (also: NAVIGATOR_NO_ANIMATIONS=1)\n"
         "  -V, --version     print the version and exit\n"
         "  -h, --help        print this help and exit\n"
         "  --                end of options; the next argument is the PATH\n";
    return s;
}

std::string_view version_string() noexcept { return NAVIGATOR_VERSION_STRING; }

} // namespace navigator::app
