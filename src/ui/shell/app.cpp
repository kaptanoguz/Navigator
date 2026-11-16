// src/ui/shell/app.cpp — navigator::App (see navigator/ui.hpp).
//
// Frame order (docs/architecture.md, "Frame model" and "Phase 2 browser data flow"):
//   1. frame start: BrowserModel::frame_start() applies finished listings and watch batches;
//   2. input: SDL events -> KeyChord -> resolve_command -> CommandQueue; mouse -> PointerInput;
//   3. build + draw: BrowserView::update() then draw() (reads the model; records Intents);
//   4. present;
//   5. frame end: apply pointer Intents, then drain the command queue.
// Workers (listing, watch) wake the loop with an SDL user event.
//
// Pacing: with the FPS overlay visible, during an animation (theme crossfade, smooth scroll,
// pointer drag) or in max_frames mode the loop runs continuously (vsync). Otherwise it blocks in
// SDL_WaitEventTimeout and redraws only when an event, a worker or a command dirtied the frame.
//
// Development-only environment (not part of the contract; set by navigator's hidden flags):
//   NAVIGATOR_DEBUG_SCREENSHOT=<path.bmp>  save the last frame before present (needs max_frames)
//   NAVIGATOR_DEBUG_SCALE=<float>          override the display scale (window size follows)
//   NAVIGATOR_DEBUG_SCRIPT=<steps>         scripted UI steps before frames are counted,
//   ';'-separated:
//                                          cmd:NAME[:ARG]  click:NAME[:ctrl|shift|ctrlshift]
//                                          hover:NAME  go:PATH  blur
//   NAVIGATOR_DEBUG_BENCH_SCROLL=1         scroll benchmark: logs frame times to stdout and quits
// User-facing: NAVIGATOR_NO_ANIMATIONS=1 forces AppConfig::animations off.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <navigator/i18n.hpp>
#include <navigator/platform.hpp>
#include <navigator/ui.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "browser/browser_view.hpp"
#include "input/sdl_input.hpp"
#include "render/backend_name.hpp"
#include "render/renderer.hpp"
#include "render/sdl_handles.hpp"
#include "shell/painter.hpp"
#include "shell/ui_settings.hpp"
#include "text/font_cache.hpp"
#include "text/text_renderer.hpp"
#include "theme/theme.hpp"
#include "widgets/ui_context.hpp"

#ifndef NAVIGATOR_VERSION_STRING
#define NAVIGATOR_VERSION_STRING "0.0.0"
#endif

namespace navigator {
namespace {

constexpr const char* kRenderDriverPreference = "gpu,vulkan,opengl,software";
constexpr int kMinWindowWidth = 720; // logical px
constexpr int kMinWindowHeight = 480;
constexpr Sint32 kIdleWaitMs = 1000;
constexpr double kFpsSmoothing = 0.08;      // EMA weight of the newest frame
constexpr double kOverlayRefreshMs = 250.0; // how often the overlay numbers change

const char* env(const char* name) noexcept {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read at startup on the UI thread.
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? v : nullptr;
}

double now_ms() noexcept { return static_cast<double>(SDL_GetTicksNS()) / 1.0e6; }

float ease_out_cubic(float t) noexcept {
    const float u = 1.0F - t;
    return 1.0F - (u * u * u);
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t next = s.find(sep, pos);
        const std::size_t end = next == std::string_view::npos ? s.size() : next;
        if (end > pos) {
            out.emplace_back(s.substr(pos, end - pos));
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }
    return out;
}

bool is_cursor_command(CommandId id) noexcept {
    const auto v = static_cast<std::uint32_t>(id);
    return v >= static_cast<std::uint32_t>(CommandId::CursorUp) &&
           v <= static_cast<std::uint32_t>(CommandId::CursorEnd);
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto i = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
    return v[i];
}

double mean(const std::vector<double>& v) {
    double s = 0.0;
    for (const double x : v) {
        s += x;
    }
    return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

} // namespace

struct App::Impl {
    // Declaration order is destruction order reversed: SDL is torn down last, the model (and
    // its worker threads, which push SDL events) first.
    ui::SdlVideo video;
    ui::WindowPtr window;
    ui::RendererPtr sdl_renderer;
    std::unique_ptr<ui::Renderer> renderer;
    std::unique_ptr<ui::text::FontCache> fonts;
    std::unique_ptr<ui::text::GlyphAtlas> atlas;
    std::unique_ptr<ui::text::TextRenderer> text;

    CommandQueue queue;
    CommandRegistry registry;
    Keymap keymap;

    std::unique_ptr<platform::IAppLauncher> launcher; // outlives the model
    std::unique_ptr<ui::BrowserModel> model;
    ui::BrowserView view;
    ui::PointerInput pointer;
    std::vector<ui::Intent> intents;
    Path home;
    Uint32 wake_event = 0;
    bool focused = true;
    ui::Density density = ui::Density::Compact; // from ui::ui_settings() at create

    std::string title;
    int max_frames = 0;
    bool show_fps = false; // mirrors AppConfig::show_fps
    Theme theme = Theme::Dark;
    Theme previous_theme = Theme::Dark;
    double fade_start_ms = -1.0;
    bool animations = true;
    bool running = true;
    bool dirty = true;
    double last_draw_ms = -1.0;

    float scale = 1.0F;
    float pixel_density = 1.0F;
    std::optional<float> scale_override;
    std::string screenshot_path;
    std::string driver_name;

    // Development: scripted steps and the scroll benchmark.
    std::vector<std::string> script;
    std::size_t script_step = 0;
    bool bench = false;
    int bench_phase = 0;
    std::vector<double> bench_jump;
    std::vector<double> bench_smooth;
    std::vector<double> bench_build; // update + draw (UI code, before present), all phases
    std::uint64_t counted_frames = 0;

    FrameStats stats;
    bool stats_reset = true;
    double shown_fps = 0.0;
    double shown_ms = 0.0;
    double shown_at_ms = -1.0e9;

    Result<void> init(const AppConfig& config);
    void handle_event(const SDL_Event& event);
    void execute(const Command& command);
    void apply_intents();
    void run_script_step();
    [[nodiscard]] bool counting() const noexcept;
    [[nodiscard]] bool continuous() const noexcept;
    [[nodiscard]] float fade_progress(double t_ms) const noexcept;
    void update_scale();
    void draw_frame(double t_ms, double dt_ms);
    void bench_step(double frame_ms, double build_ms);
    void save_screenshot() const;
    void update_stats(double frame_ms, double t_ms);
};

Result<void> App::Impl::init(const AppConfig& config) {
    title = std::string(config.title);
    max_frames = config.max_frames;
    show_fps = config.show_fps;
    theme = config.theme;
    previous_theme = config.theme;
    animations = config.animations && env("NAVIGATOR_NO_ANIMATIONS") == nullptr;
    density = ui::ui_settings().density;
    if (const char* s = env("NAVIGATOR_DEBUG_SCREENSHOT")) {
        screenshot_path = s;
    }
    if (const char* s = env("NAVIGATOR_DEBUG_SCALE")) {
        const float v = std::strtof(s, nullptr);
        if (v >= 0.5F && v <= 4.0F) {
            scale_override = v;
        }
    }
    if (const char* s = env("NAVIGATOR_DEBUG_SCRIPT")) {
        script = split(s, ';');
    }
    bench = env("NAVIGATOR_DEBUG_BENCH_SCROLL") != nullptr;

    if (auto r = register_builtin_commands(registry); !r) {
        return r;
    }
    if (auto r = load_default_keymap(keymap); !r) {
        SDL_Log("Navigator: default keymap failed to load: %s", r.error().message.c_str());
    }

    // Fonts first: with no usable font we refuse to start before any window appears.
    auto font_cache = ui::text::FontCache::create();
    if (!font_cache) {
        return fail(font_cache.error().code, font_cache.error().message);
    }
    fonts = std::move(*font_cache);

    SDL_SetAppMetadata("Navigator", NAVIGATOR_VERSION_STRING, "navigator");
    // Respect SDL_RENDER_DRIVER from the environment; otherwise prefer SDL_GPU (Vulkan).
    if (SDL_GetHint(SDL_HINT_RENDER_DRIVER) == nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, kRenderDriverPreference);
    }
    if (!video.init()) {
        return fail(ErrorCode::PlatformError, std::string("SDL_Init failed: ") + SDL_GetError());
    }
    wake_event = SDL_RegisterEvents(1);

    const SDL_WindowFlags flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    window.reset(SDL_CreateWindow(title.c_str(), config.width, config.height, flags));
    if (!window) {
        return fail(ErrorCode::PlatformError,
                    std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    // On X11 (and Wayland without fractional-scale support) the content scale is not part of the
    // pixel density: grow the window so the requested logical size holds at that scale.
    const float pixel_ratio = std::max(SDL_GetWindowPixelDensity(window.get()), 0.01F);
    float content = SDL_GetWindowDisplayScale(window.get()) / pixel_ratio;
    if (scale_override) {
        content = *scale_override / pixel_ratio;
    }
    if (content > 1.01F) {
        SDL_SetWindowSize(
            window.get(), static_cast<int>(std::lround(static_cast<float>(config.width) * content)),
            static_cast<int>(std::lround(static_cast<float>(config.height) * content)));
    }
    SDL_SetWindowMinimumSize(window.get(), kMinWindowWidth, kMinWindowHeight);

    sdl_renderer.reset(SDL_CreateRenderer(window.get(), nullptr));
    if (!sdl_renderer) {
        return fail(ErrorCode::PlatformError,
                    std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
    }
    if (!SDL_SetRenderVSync(sdl_renderer.get(), bench ? 0 : 1)) {
        SDL_Log("Navigator: vsync unavailable (%s)", SDL_GetError());
    }
    const char* name = SDL_GetRendererName(sdl_renderer.get());
    const std::string renderer_id = name != nullptr ? name : "";
    std::string gpu_driver;
    if (renderer_id == "gpu") {
        const SDL_PropertiesID props = SDL_GetRendererProperties(sdl_renderer.get());
        auto* device = static_cast<SDL_GPUDevice*>(
            SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
        if (device != nullptr) {
            if (const char* d = SDL_GetGPUDeviceDriver(device); d != nullptr) {
                gpu_driver = d;
            }
        }
    }
    driver_name = ui::backend_display_name(renderer_id, gpu_driver);
    const char* video_driver = SDL_GetCurrentVideoDriver();
    SDL_Log("Navigator %s: video driver '%s', render driver '%s'%s%s, font '%s' (%s)",
            NAVIGATOR_VERSION_STRING, video_driver != nullptr ? video_driver : "none",
            renderer_id.c_str(), gpu_driver.empty() ? "" : " on ", gpu_driver.c_str(),
            fonts->regular_file().family.c_str(), fonts->regular_file().path.c_str());

    renderer = std::make_unique<ui::Renderer>(sdl_renderer.get());
    auto glyph_atlas = ui::text::GlyphAtlas::create(sdl_renderer.get());
    if (!glyph_atlas) {
        return fail(glyph_atlas.error().code, glyph_atlas.error().message);
    }
    atlas = std::move(*glyph_atlas);
    text = std::make_unique<ui::text::TextRenderer>(*renderer, *atlas);
    update_scale();

    // Browser.
    if (auto h = platform::user_dir(platform::UserDir::Home); h) {
        home = *h;
    }
    ui::BrowserModel::Options options;
    if (auto l = platform::create_default_launcher(); l) {
        launcher = std::move(*l);
    } else {
        SDL_Log("Navigator: no app launcher (%s); files cannot be opened",
                l.error().message.c_str());
    }
    options.launcher = launcher.get();
    options.watcher_factory = [] { return create_inotify_watcher(); };
    const Uint32 wake_type = wake_event;
    options.wake = [wake_type] {
        if (wake_type == 0) {
            return;
        }
        SDL_Event e{};
        e.type = wake_type;
        SDL_PushEvent(&e); // thread-safe
    };
    model = std::make_unique<ui::BrowserModel>(std::move(options));
    Path start{std::string(config.start_path)};
    if (start.empty()) {
        start = home.empty() ? Path("/") : home;
    }
    model->open_start(start);
    return ok();
}

bool App::Impl::counting() const noexcept {
    // Scripted and screenshot runs count frames only once the browser has settled.
    if (script.empty() && screenshot_path.empty()) {
        return true;
    }
    return script_step >= script.size() && model->loaded() && !model->busy();
}

bool App::Impl::continuous() const noexcept {
    return max_frames > 0 || show_fps || fade_start_ms >= 0.0 || view.animating() || bench;
}

float App::Impl::fade_progress(double t_ms) const noexcept {
    if (fade_start_ms < 0.0) {
        return 1.0F;
    }
    const double t = (t_ms - fade_start_ms) / ui::motion::theme_crossfade_ms;
    return ease_out_cubic(static_cast<float>(std::clamp(t, 0.0, 1.0)));
}

void App::Impl::handle_event(const SDL_Event& event) {
    dirty = true;
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: queue.enqueue(CommandId::Quit); break;
    case SDL_EVENT_KEY_DOWN: {
        pointer.mods = ui::translate_modifiers(event.key.mod);
        const KeyChord chord =
            ui::translate_chord(event.key.key, event.key.scancode, event.key.mod);
        if (chord.key == Key::Unknown) {
            break;
        }
        if (const auto cmd = resolve_command(keymap, registry, chord); cmd) {
            // Auto-repeat moves the cursor; it never re-fires toggles or navigation.
            if (!event.key.repeat || is_cursor_command(cmd->id)) {
                queue.enqueue(*cmd);
            }
        }
        break;
    }
    case SDL_EVENT_KEY_UP: pointer.mods = ui::translate_modifiers(event.key.mod); break;
    case SDL_EVENT_MOUSE_MOTION:
        pointer.x = event.motion.x * pixel_density;
        pointer.y = event.motion.y * pixel_density;
        pointer.inside = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        pointer.x = event.button.x * pixel_density;
        pointer.y = event.button.y * pixel_density;
        pointer.mods = ui::translate_modifiers(SDL_GetModState());
        if (event.button.button == SDL_BUTTON_LEFT) {
            pointer.down = true;
            pointer.pressed = true;
            pointer.clicks = event.button.clicks;
        } else if (const Key k = ui::translate_mouse_button(event.button.button);
                   k != Key::Unknown) {
            if (const auto cmd = resolve_command(keymap, registry, KeyChord{k, pointer.mods});
                cmd) {
                queue.enqueue(*cmd);
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        pointer.x = event.button.x * pixel_density;
        pointer.y = event.button.y * pixel_density;
        if (event.button.button == SDL_BUTTON_LEFT) {
            pointer.down = false;
            pointer.released = true;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float y = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            y = -y;
        }
        pointer.wheel_y += y;
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: pointer.inside = false; break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER: pointer.inside = true; break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: focused = true; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        focused = false;
        if (pointer.down) {
            pointer.down = false;
            pointer.released = true; // never leave a drag captured
        }
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: update_scale(); break;
    default: break;
    }
}

void App::Impl::execute(const Command& command) {
    dirty = true;
    if (model->execute(command)) {
        return;
    }
    switch (command.id) {
    case CommandId::None: break;
    case CommandId::Quit: running = false; break;
    case CommandId::ToggleFpsOverlay:
        show_fps = !show_fps;
        stats_reset = true;
        break;
    case CommandId::ToggleTheme:
        previous_theme = theme;
        theme = ui::toggled(theme);
        fade_start_ms = animations ? now_ms() : -1.0;
        break;
    // Browser commands are handled by BrowserModel::execute above.
    case CommandId::ToggleHidden:
    case CommandId::NavigateBack:
    case CommandId::NavigateForward:
    case CommandId::NavigateUp:
    case CommandId::Refresh:
    case CommandId::FocusAddressBar:
    case CommandId::Activate:
    case CommandId::SelectAll:
    case CommandId::ClearSelection:
    case CommandId::CursorUp:
    case CommandId::CursorDown:
    case CommandId::CursorPageUp:
    case CommandId::CursorPageDown:
    case CommandId::CursorHome:
    case CommandId::CursorEnd: break;
    }
}

void App::Impl::apply_intents() {
    for (const ui::Intent& in : intents) {
        dirty = true;
        switch (in.kind) {
        case ui::Intent::Kind::ClickRow: model->click_row(in.row, in.mods); break;
        case ui::Intent::Kind::BandBegin: model->band_begin(in.additive); break;
        case ui::Intent::Kind::BandUpdate: model->band_update(in.row, in.row_last); break;
        case ui::Intent::Kind::BandEnd: model->band_end(); break;
        case ui::Intent::Kind::ClearSelection: model->clear_selection(); break;
        case ui::Intent::Kind::SortBy: model->sort_by(in.sort_key); break;
        case ui::Intent::Kind::NavigateTo: model->navigate_to(in.path); break;
        case ui::Intent::Kind::DismissNotice: model->dismiss_notice(); break;
        }
    }
    intents.clear();
}

void App::Impl::run_script_step() {
    if (script_step >= script.size() || !model->loaded() || model->busy()) {
        return;
    }
    const std::vector<std::string> parts = split(script[script_step], ':');
    ++script_step;
    dirty = true;
    if (parts.empty()) {
        return;
    }
    const std::string& op = parts[0];
    const std::string arg = parts.size() > 1 ? parts[1] : std::string();
    if (op == "cmd") {
        if (const CommandInfo* info = registry.find(arg); info != nullptr) {
            std::uint64_t value = 0;
            if (parts.size() > 2) {
                value = std::strtoull(parts[2].c_str(), nullptr, 10);
            }
            queue.enqueue(info->id, value);
        } else {
            SDL_Log("Navigator: script: unknown command '%s'", arg.c_str());
        }
    } else if (op == "click") {
        const std::size_t row = model->find(arg);
        Modifiers mods = Modifiers::None;
        if (parts.size() > 2) {
            if (parts[2].find("ctrl") != std::string::npos) {
                mods |= Modifiers::Ctrl;
            }
            if (parts[2].find("shift") != std::string::npos) {
                mods |= Modifiers::Shift;
            }
        }
        if (row != ui::kNoRow) {
            model->click_row(row, mods);
        }
    } else if (op == "hover") {
        view.set_hover_override(model->find(arg));
    } else if (op == "go") {
        model->navigate_to(Path(script[script_step - 1].substr(3)));
    } else if (op == "blur") {
        focused = false;
    }
}

void App::Impl::update_scale() {
    pixel_density = std::max(SDL_GetWindowPixelDensity(window.get()), 0.01F);
    const float s = scale_override ? *scale_override : SDL_GetWindowDisplayScale(window.get());
    const float next = s > 0.0F ? s : 1.0F;
    if (std::abs(next - scale) > 0.001F) {
        scale = next;
        // Re-rasterize at the new pixel size.
        fonts->clear();
        atlas->clear();
        view.clear_caches();
    }
}

void App::Impl::draw_frame(double t_ms, double dt_ms) {
    int w = 0;
    int h = 0;
    SDL_GetRenderOutputSize(sdl_renderer.get(), &w, &h);

    const float fade = fade_progress(t_ms);
    if (fade >= 1.0F) {
        fade_start_ms = -1.0;
    }
    const ui::Palette pal = fade < 1.0F
                                ? ui::mix(ui::palette(previous_theme), ui::palette(theme), fade)
                                : ui::palette(theme);
    const std::string_view lang = language() == Language::Turkish ? "tr" : "en";
    const ui::Painter painter(*renderer, *text, *fonts, scale, lang);
    const auto now =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    ui::BrowserFrame frame{
        .painter = painter,
        .palette = pal,
        .theme = theme,
        .model = *model,
        .keymap = keymap,
        .pointer = pointer,
        .queue = queue,
        .intents = intents,
        .width = static_cast<float>(w),
        .height = static_cast<float>(h),
        .focused = focused,
        .animations = animations,
        .density = density,
        .dt_ms = dt_ms,
        .now = now,
        .home = home.empty() ? nullptr : &home,
        .show_fps = show_fps,
        .fps = shown_fps,
        .frame_ms = shown_ms,
        .render_driver = driver_name,
    };
    view.update(frame);
    view.draw(frame);
}

void App::Impl::bench_step(double frame_ms, double build_ms) {
    // Phase 0: wait for the listing. 1: one page per frame to the end (every row new each
    // frame, the worst case for shaping). 2: smooth 6 dp per frame from the top for 600 frames.
    if (!model->loaded() || model->busy()) {
        return;
    }
    switch (bench_phase) {
    case 0:
        bench_phase = 1;
        view.scroll_by(-view.max_scroll());
        break;
    case 1:
        bench_jump.push_back(frame_ms);
        bench_build.push_back(build_ms);
        if (view.scroll() >= view.max_scroll() || bench_jump.size() >= 4000) {
            bench_phase = 2;
            view.scroll_by(-view.max_scroll());
        } else {
            view.scroll_by(static_cast<float>(view.page_rows()) *
                           ui::density_metrics(density).row_height);
        }
        break;
    case 2:
        bench_smooth.push_back(frame_ms);
        bench_build.push_back(build_ms);
        view.scroll_by(6.0F);
        if (bench_smooth.size() >= 600) {
            std::printf("bench-scroll: entries=%zu driver=%s scale=%.2f\n", model->entries().size(),
                        driver_name.c_str(), static_cast<double>(scale));
            std::printf("bench-scroll: page-per-frame frames=%zu avg=%.2fms p50=%.2fms "
                        "p95=%.2fms max=%.2fms\n",
                        bench_jump.size(), mean(bench_jump), percentile(bench_jump, 0.5),
                        percentile(bench_jump, 0.95), percentile(bench_jump, 1.0));
            std::printf("bench-scroll: smooth-6dp frames=%zu avg=%.2fms p50=%.2fms p95=%.2fms "
                        "max=%.2fms\n",
                        bench_smooth.size(), mean(bench_smooth), percentile(bench_smooth, 0.5),
                        percentile(bench_smooth, 0.95), percentile(bench_smooth, 1.0));
            std::printf("bench-scroll: ui-build (update+draw, before present) avg=%.2fms "
                        "p95=%.2fms\n",
                        mean(bench_build), percentile(bench_build, 0.95));
            (void)std::fflush(stdout);
            running = false;
        }
        break;
    default: break;
    }
}

void App::Impl::save_screenshot() const {
    ui::SurfacePtr shot{SDL_RenderReadPixels(sdl_renderer.get(), nullptr)};
    if (!shot) {
        SDL_Log("Navigator: screenshot read failed: %s", SDL_GetError());
        return;
    }
    if (SDL_SaveBMP(shot.get(), screenshot_path.c_str())) {
        SDL_Log("Navigator: screenshot saved to %s (%dx%d)", screenshot_path.c_str(), shot->w,
                shot->h);
    } else {
        SDL_Log("Navigator: screenshot save failed: %s", SDL_GetError());
    }
}

void App::Impl::update_stats(double frame_ms, double t_ms) {
    stats.frame_ms = frame_ms;
    if (stats_reset || stats.fps <= 0.0) {
        stats.fps = frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0;
        stats_reset = false;
    } else if (frame_ms > 0.0) {
        stats.fps += ((1000.0 / frame_ms) - stats.fps) * kFpsSmoothing;
    }
    if (t_ms - shown_at_ms >= kOverlayRefreshMs) {
        shown_fps = stats.fps;
        shown_ms = stats.fps > 0.0 ? 1000.0 / stats.fps : 0.0;
        shown_at_ms = t_ms;
    }
}

App::App(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
App::~App() = default;

Result<std::unique_ptr<App>> App::create(const AppConfig& config) {
    if (config.width <= 0 || config.height <= 0) {
        return fail(ErrorCode::InvalidArgument, "window size must be positive");
    }
    if (config.max_frames < 0) {
        return fail(ErrorCode::InvalidArgument, "max_frames must not be negative");
    }
    auto impl = std::make_unique<Impl>();
    if (auto r = impl->init(config); !r) {
        return fail(r.error().code, r.error().message);
    }
    return std::unique_ptr<App>(new App(std::move(impl)));
}

int App::run() {
    Impl& s = *impl_;
    SDL_ShowWindow(s.window.get());
    double last_ms = now_ms();
    bool first = true;
    // Scripted/screenshot runs: give the listing a bounded time to settle.
    const double settle_deadline = now_ms() + 10000.0;
    while (s.running) {
        // 1. Wait for input (idle) or poll, then apply finished background work.
        SDL_Event event;
        if (!first && !s.continuous() && !s.dirty) {
            if (SDL_WaitEventTimeout(&event, kIdleWaitMs)) {
                s.handle_event(event);
            }
        }
        if (s.model->frame_start()) {
            s.dirty = true;
        }
        while (SDL_PollEvent(&event)) {
            s.handle_event(event);
        }
        s.run_script_step();
        if (!s.dirty && !s.continuous() && s.queue.empty()) {
            continue; // woke up without anything to show
        }

        // 2-3. Build/draw the frame, then present.
        const double t_ms = now_ms();
        const double dt_ms = s.last_draw_ms < 0.0 ? 16.0 : std::min(t_ms - s.last_draw_ms, 100.0);
        s.last_draw_ms = t_ms;
        s.draw_frame(t_ms, dt_ms);
        const double built_ms = now_ms();
        const bool settled = s.counting() || t_ms > settle_deadline;
        const bool last_frame = s.max_frames > 0 && settled &&
                                s.counted_frames + 1 >= static_cast<std::uint64_t>(s.max_frames);
        if (last_frame && !s.screenshot_path.empty()) {
            s.save_screenshot();
        }
        SDL_RenderPresent(s.sdl_renderer.get());
        s.dirty = false;
        first = false;

        // 4. Frame end: pointer intents, then queued commands (FIFO; commands they enqueue run
        //    next frame).
        s.apply_intents();
        s.model->set_page_rows(s.view.page_rows());
        s.model->set_scroll(s.view.scroll());
        s.queue.drain([&s](const Command& c) { s.execute(c); });
        s.pointer.pressed = false;
        s.pointer.released = false;
        s.pointer.clicks = 0;
        s.pointer.wheel_y = 0.0F;

        const double end_ms = now_ms();
        if (s.bench) {
            s.bench_step(end_ms - t_ms, built_ms - t_ms);
        }
        s.update_stats(end_ms - last_ms, end_ms);
        last_ms = end_ms;
        ++s.stats.frame_index;
        if (settled) {
            ++s.counted_frames;
        }
        if (last_frame) {
            break;
        }
    }
    SDL_HideWindow(s.window.get());
    return 0;
}

CommandQueue& App::commands() noexcept { return impl_->queue; }

FrameStats App::frame_stats() const noexcept { return impl_->stats; }

Theme App::theme() const noexcept { return impl_->theme; }

} // namespace navigator
