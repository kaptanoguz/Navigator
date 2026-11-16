// src/ui/shell/app.cpp — navigator::App (see navigator/ui.hpp).
//
// Frame order: poll events -> build/draw scene -> present -> drain the command queue.
// Widgets and input only enqueue CommandIds; App executes them after the frame is presented.
//
// Pacing: with the FPS overlay visible, during an animation, or in max_frames mode the loop runs
// continuously (vsync). Otherwise it blocks in SDL_WaitEventTimeout and redraws only when an
// event or a command dirtied the frame, so an idle window costs ~0% CPU.
//
// Development-only environment (not part of the contract; set by `navigator --screenshot/--scale`):
//   NAVIGATOR_DEBUG_SCREENSHOT=<path.bmp>  save the last frame before present (needs max_frames)
//   NAVIGATOR_DEBUG_SCALE=<float>          override the display scale (window size follows)
//   NAVIGATOR_NO_ANIMATIONS=1              disable the theme crossfade
// NAVIGATOR_NO_ANIMATIONS is also user-facing: `navigator --no-animations` sets it (AppConfig has
// no field for it yet).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include <navigator/i18n.hpp>
#include <navigator/ui.hpp>

#include "render/backend_name.hpp"
#include "render/renderer.hpp"
#include "render/sdl_handles.hpp"
#include "shell/background.hpp"
#include "shell/keyboard.hpp"
#include "shell/painter.hpp"
#include "shell/scene.hpp"
#include "text/font_cache.hpp"
#include "text/text_renderer.hpp"
#include "theme/theme.hpp"

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

} // namespace

struct App::Impl {
    // Declaration order is destruction order reversed: SDL is torn down last.
    ui::SdlVideo video;
    ui::WindowPtr window;
    ui::RendererPtr sdl_renderer;
    std::unique_ptr<ui::Renderer> renderer;
    std::unique_ptr<ui::text::FontCache> fonts;
    std::unique_ptr<ui::text::GlyphAtlas> atlas;
    std::unique_ptr<ui::text::TextRenderer> text;
    std::array<ui::TexturePtr, 2> backdrops; // indexed by Theme
    int backdrop_w = 0;
    int backdrop_h = 0;

    CommandQueue queue;
    CommandRegistry registry;

    std::string title;
    int max_frames = 0;
    bool show_fps = false; // mirrors AppConfig::show_fps
    Theme theme = Theme::Dark;
    Theme previous_theme = Theme::Dark;
    double fade_start_ms = -1.0;
    bool animations = true;
    bool running = true;
    bool dirty = true;

    float scale = 1.0F;
    std::optional<float> scale_override;
    std::string screenshot_path;
    std::string driver_name;

    FrameStats stats;
    bool stats_reset = true;
    double shown_fps = 0.0;
    double shown_ms = 0.0;
    double shown_at_ms = -1.0e9;

    Result<void> init(const AppConfig& config);
    void handle_event(const SDL_Event& event);
    void execute(const Command& command);
    [[nodiscard]] bool continuous() const noexcept;
    [[nodiscard]] float fade_progress(double t_ms) const noexcept;
    void update_scale();
    void ensure_backdrops(int w, int h);
    void draw_frame(double t_ms);
    void save_screenshot() const;
    void update_stats(double frame_ms, double t_ms);
};

Result<void> App::Impl::init(const AppConfig& config) {
    title = std::string(config.title);
    max_frames = config.max_frames;
    show_fps = config.show_fps;
    theme = config.theme;
    previous_theme = config.theme;
    animations = env("NAVIGATOR_NO_ANIMATIONS") == nullptr;
    if (const char* s = env("NAVIGATOR_DEBUG_SCREENSHOT")) {
        screenshot_path = s;
    }
    if (const char* s = env("NAVIGATOR_DEBUG_SCALE")) {
        const float v = std::strtof(s, nullptr);
        if (v >= 0.5F && v <= 4.0F) {
            scale_override = v;
        }
    }

    if (auto r = register_builtin_commands(registry); !r) {
        return r;
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

    const SDL_WindowFlags flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    window.reset(SDL_CreateWindow(title.c_str(), config.width, config.height, flags));
    if (!window) {
        return fail(ErrorCode::PlatformError,
                    std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    // On X11 (and Wayland without fractional-scale support) the content scale is not part of the
    // pixel density: grow the window so the requested logical size holds at that scale.
    const float density = std::max(SDL_GetWindowPixelDensity(window.get()), 0.01F);
    float content = SDL_GetWindowDisplayScale(window.get()) / density;
    if (scale_override) {
        content = *scale_override / density;
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
    if (!SDL_SetRenderVSync(sdl_renderer.get(), 1)) {
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
    return ok();
}

bool App::Impl::continuous() const noexcept {
    return max_frames > 0 || show_fps || fade_start_ms >= 0.0;
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
        if (event.key.repeat) {
            break;
        }
        const ui::Key key = ui::translate_key(event.key.key, event.key.scancode);
        const ui::Modifiers m = ui::translate_modifiers(event.key.mod);
        const CommandId id = ui::resolve_binding(registry, key, m);
        if (id != CommandId::None) {
            queue.enqueue(id);
        }
        break;
    }
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: update_scale(); break;
    default: break;
    }
}

void App::Impl::execute(const Command& command) {
    dirty = true;
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
    }
}

void App::Impl::update_scale() {
    const float s = scale_override ? *scale_override : SDL_GetWindowDisplayScale(window.get());
    const float next = s > 0.0F ? s : 1.0F;
    if (std::abs(next - scale) > 0.001F) {
        scale = next;
        // Re-rasterize at the new pixel size.
        fonts->clear();
        atlas->clear();
    }
}

void App::Impl::ensure_backdrops(int w, int h) {
    if (w == backdrop_w && h == backdrop_h && backdrops[0] && backdrops[1]) {
        return;
    }
    backdrop_w = w;
    backdrop_h = h;
    for (const Theme t : {Theme::Dark, Theme::Light}) {
        auto tex = ui::make_backdrop_texture(sdl_renderer.get(), ui::palette(t), w, h);
        backdrops[static_cast<std::size_t>(t)] = tex ? std::move(*tex) : nullptr;
    }
}

void App::Impl::draw_frame(double t_ms) {
    int w = 0;
    int h = 0;
    SDL_GetRenderOutputSize(sdl_renderer.get(), &w, &h);
    ensure_backdrops(w, h);

    const float fade = fade_progress(t_ms);
    if (fade >= 1.0F) {
        fade_start_ms = -1.0;
    }
    ui::SceneModel model;
    model.palette = fade < 1.0F ? ui::mix(ui::palette(previous_theme), ui::palette(theme), fade)
                                : ui::palette(theme);
    model.width = static_cast<float>(w);
    model.height = static_cast<float>(h);
    model.background_from =
        fade < 1.0F ? backdrops[static_cast<std::size_t>(previous_theme)].get() : nullptr;
    model.background_to = backdrops[static_cast<std::size_t>(theme)].get();
    model.background_t = fade;
    model.show_fps = show_fps;
    model.fps = shown_fps;
    model.frame_ms = shown_ms;
    model.render_driver = driver_name;

    const std::string_view lang = language() == Language::Turkish ? "tr" : "en";
    const ui::Painter painter(*renderer, *text, *fonts, scale, lang);
    ui::draw_scene(painter, model);
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
    while (s.running) {
        // 1. Input.
        SDL_Event event;
        if (!first && !s.continuous() && !s.dirty) {
            if (SDL_WaitEventTimeout(&event, kIdleWaitMs)) {
                s.handle_event(event);
            }
        }
        while (SDL_PollEvent(&event)) {
            s.handle_event(event);
        }
        if (!s.dirty && !s.continuous() && s.queue.empty()) {
            continue; // woke up without anything to show
        }

        // 2-3. Build/draw the scene, then present.
        const double t_ms = now_ms();
        s.draw_frame(t_ms);
        const bool last_frame =
            s.max_frames > 0 && s.stats.frame_index + 1 >= static_cast<std::uint64_t>(s.max_frames);
        if (last_frame && !s.screenshot_path.empty()) {
            s.save_screenshot();
        }
        SDL_RenderPresent(s.sdl_renderer.get());
        s.dirty = false;
        first = false;

        // 4. Frame end: run queued commands (FIFO; commands they enqueue run next frame).
        s.queue.drain([&s](const Command& c) { s.execute(c); });

        const double end_ms = now_ms();
        s.update_stats(end_ms - last_ms, end_ms);
        last_ms = end_ms;
        ++s.stats.frame_index;
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
