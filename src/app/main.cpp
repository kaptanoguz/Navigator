// src/app/main.cpp — navigator entry point: parse arguments, pick the UI language, run the App.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/i18n.hpp>
#include <navigator/platform.hpp>
#include <navigator/ui.hpp>

#include "bench_fixture.hpp"
#include "cli.hpp"
#include "shell/ui_settings.hpp"

namespace {

std::string_view env_or_empty(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded at this point.
    const char* v = std::getenv(name);
    return v != nullptr ? std::string_view(v) : std::string_view();
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view program = argc > 0 ? std::string_view(argv[0]) : "navigator";
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    auto options = navigator::app::parse_cli(args);
    if (!options) {
        std::fprintf(stderr, "navigator: %s\nTry '%s --help'.\n", options.error().message.c_str(),
                     std::string(program).c_str());
        return 2;
    }
    if (options->show_help) {
        std::fputs(navigator::app::usage_text(program).c_str(), stdout);
        return 0;
    }
    if (options->show_version) {
        std::printf("navigator %s\n", std::string(navigator::app::version_string()).c_str());
        return 0;
    }

    navigator::set_language(options->language.value_or(
        navigator::app::detect_language(env_or_empty("LANGUAGE"), env_or_empty("LC_ALL"),
                                        env_or_empty("LC_MESSAGES"), env_or_empty("LANG"))));

    // Phase 10: Single-instance IPC forwarding. If another Navigator instance is already
    // listening on the socket, forward --new-tab / --select / PATH and exit cleanly.
    const bool try_ipc = options->config.enable_ipc && !options->new_instance &&
                         (options->config.max_frames == 0 || !options->ipc_socket.empty());
    if (try_ipc) {
        std::optional<navigator::Path> sock_path;
        if (!options->ipc_socket.empty()) {
            sock_path = navigator::Path(options->ipc_socket);
        } else if (auto def = navigator::platform::default_ipc_socket_path(); def) {
            sock_path = std::move(*def);
        }
        if (sock_path) {
            auto abs_path = [](const std::string& raw) -> navigator::Path {
                if (raw.empty()) {
                    return {};
                }
                if (navigator::platform::is_gvfs_uri(raw)) {
                    return {raw};
                }
                std::error_code ec;
                navigator::Path p(raw);
                if (!p.is_absolute()) {
                    p = std::filesystem::current_path(ec) / p;
                }
                return p.lexically_normal();
            };

            navigator::platform::IpcMessage msg;
            if (!options->select_path.empty()) {
                msg.action = navigator::platform::IpcAction::SelectPath;
                msg.path = abs_path(options->select_path);
            } else if (options->new_tab) {
                msg.action = navigator::platform::IpcAction::NewTab;
                msg.path = abs_path(options->start_path);
            } else if (!options->start_path.empty()) {
                msg.action = navigator::platform::IpcAction::OpenPath;
                msg.path = abs_path(options->start_path);
            } else {
                msg.action = navigator::platform::IpcAction::FocusWindow;
            }
            if (auto sent = navigator::platform::send_to_running_instance(*sock_path, msg);
                sent && *sent) {
                return 0;
            }
        }
    }

    // Development-only switches reach the UI through its documented debug environment
    // (src/ui/shell/app.cpp). --no-animations is AppConfig::animations.
    // NOLINTBEGIN(concurrency-mt-unsafe): single-threaded at this point.
    if (!options->screenshot_path.empty()) {
        setenv("NAVIGATOR_DEBUG_SCREENSHOT", options->screenshot_path.c_str(), 1);
    }
    if (const std::optional<float> scale = options->scale; scale.has_value()) {
        setenv("NAVIGATOR_DEBUG_SCALE", std::to_string(scale.value()).c_str(), 1);
    }
    if (!options->debug_script.empty()) {
        setenv("NAVIGATOR_DEBUG_SCRIPT", options->debug_script.c_str(), 1);
    }
    std::optional<navigator::app::BenchFixture> bench;
    if (options->bench_scroll > 0 || options->bench_flatten > 0) {
        // Benchmarks run on generated fixtures only: no index of the real volumes, no session.
        options->config.enable_index = false;
        options->config.restore_session = false;
        options->config.save_session = false;
    }
    if (options->bench_flatten > 0) {
        setenv("NAVIGATOR_DEBUG_BENCH_FLATTEN", "1", 1);
        if (options->start_path.empty()) {
            auto fx = navigator::app::create_bench_tree(options->bench_flatten);
            if (!fx) {
                std::fprintf(stderr, "navigator: bench tree: %s\n", fx.error().message.c_str());
                return 1;
            }
            bench = std::move(*fx);
            options->start_path = bench->root.string();
        }
    } else if (options->bench_scroll > 0) {
        setenv("NAVIGATOR_DEBUG_BENCH_SCROLL", "1", 1);
        if (options->start_path.empty()) {
            auto fx = navigator::app::create_bench_fixture(options->bench_scroll);
            if (!fx) {
                std::fprintf(stderr, "navigator: bench fixture: %s\n", fx.error().message.c_str());
                return 1;
            }
            bench = std::move(*fx);
            options->start_path = bench->root.string();
        }
    }
    // NOLINTEND(concurrency-mt-unsafe)

    navigator::ui::set_ui_settings(navigator::ui::UiSettings{
        .density = options->density == navigator::app::DensityChoice::Comfortable
                       ? navigator::ui::Density::Comfortable
                       : navigator::ui::Density::Compact,
    });
    navigator::AppConfig config = options->config;
    config.start_path = options->start_path; // `options` outlives the App
    config.config_dir = options->config_dir;
    config.cache_dir = options->cache_dir;
    config.thumbnail_dir = options->thumbnail_dir;
    config.trash_dir = options->trash_dir;
    config.ipc_socket_path = options->ipc_socket;
    config.plugin_dir = options->plugin_dir;
    config.select_path = options->select_path;
    int code = 1;
    {
        auto app = navigator::App::create(config);
        if (!app) {
            std::fprintf(stderr, "navigator: error: %s (%s)\n", app.error().message.c_str(),
                         std::string(navigator::to_string(app.error().code)).c_str());
        } else {
            code = (*app)->run();
        }
    }
    if (bench) {
        navigator::app::remove_bench_fixture(*bench);
    }
    return code;
}
