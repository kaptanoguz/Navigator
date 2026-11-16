// src/app/main.cpp — navigator entry point: parse arguments, pick the UI language, run the App.
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/i18n.hpp>
#include <navigator/ui.hpp>

#include "bench_fixture.hpp"
#include "cli.hpp"

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
    if (options->bench_scroll > 0) {
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

    navigator::AppConfig config = options->config;
    config.start_path = options->start_path; // `options` outlives the App
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
