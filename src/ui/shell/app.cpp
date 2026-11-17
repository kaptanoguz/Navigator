// src/ui/shell/app.cpp — navigator::App (see navigator/ui.hpp).
//
// Frame order (docs/architecture.md, "Frame model", "Phase 2 browser data flow", "Phase 3"):
//   1. frame start: Workspace::frame_start() applies finished listings and watch batches of the
//      live (visible) tabs;
//   2. input: SDL events -> KeyChord -> resolve_command -> CommandQueue; mouse -> PointerInput;
//      text input -> the open popup;
//   3. build + draw: WorkspaceView::update() then draw() (reads the Workspace; records Intents);
//   4. present;
//   5. frame end: apply pointer Intents (pane activation, tab clicks, drops, splitter moves,
//      list gestures), then drain the command queue (popups take their commands first), then
//      sync the tabs' persisted state and schedule the debounced session save.
// Workers (listing, watch) wake the loop with an SDL user event.
//
// Session (docs/architecture.md, "Persistence"): loaded once before the window is created
// (geometry: size + maximized; position on X11 only), saved 2 s after the last persisted change
// by a background writer, and synchronously when the loop ends (quit, window close, max_frames).
// Safety for headless runs: with max_frames > 0 and no explicit config_dir the session is
// neither read nor written, so smoke tests never touch ~/.config/navigator.
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
//                                          hover:NAME  go:PATH  blur  type:TEXT
//                                          focus:PANE  tab:PANE:TAB  hovertab:PANE:TAB
//                                          dragpreview:PANE:TAB:TO_PANE:left|right|top|bottom|centre
//                                          dump:FILE (panes and tabs, one line each)
//                                          (PANE/TAB: 0-based indices in pane/tab order)
//   NAVIGATOR_DEBUG_BENCH_SCROLL=1         scroll benchmark: logs frame times to stdout and quits
// User-facing: NAVIGATOR_NO_ANIMATIONS=1 forces AppConfig::animations off.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <navigator/i18n.hpp>
#include <navigator/layout.hpp>
#include <navigator/platform.hpp>
#include <navigator/ui.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "browser/browser_view.hpp"
#include "i18n/catalog.hpp"
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
#include "workspace/session_writer.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_view.hpp"

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

// RAII for the three system cursors the shell uses.
struct Cursors {
    SDL_Cursor* arrow = nullptr;
    SDL_Cursor* ew = nullptr;
    SDL_Cursor* ns = nullptr;
    ui::PointerCursor shown = ui::PointerCursor::Default;
    Cursors() = default;
    Cursors(const Cursors&) = delete;
    Cursors& operator=(const Cursors&) = delete;
    Cursors(Cursors&&) = delete;
    Cursors& operator=(Cursors&&) = delete;
    ~Cursors() {
        for (SDL_Cursor* c : {arrow, ew, ns}) {
            if (c != nullptr) {
                SDL_DestroyCursor(c);
            }
        }
    }
    void create() {
        arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        ew = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
        ns = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    }
    void show(ui::PointerCursor c) {
        if (c == shown) {
            return;
        }
        shown = c;
        SDL_Cursor* target = c == ui::PointerCursor::ResizeHorizontal ? ew
                             : c == ui::PointerCursor::ResizeVertical ? ns
                                                                      : arrow;
        if (target != nullptr) {
            SDL_SetCursor(target);
        }
    }
};

std::string error_text(ErrorCode code) {
    return std::string(tr("error." + std::string(to_string(code))));
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
    Cursors cursors; // destroyed before SDL_Quit
    ui::WindowPtr window;
    ui::RendererPtr sdl_renderer;
    std::unique_ptr<ui::Renderer> renderer;
    std::unique_ptr<ui::text::FontCache> fonts;
    std::unique_ptr<ui::text::GlyphAtlas> atlas;
    std::unique_ptr<ui::text::TextRenderer> text;

    CommandQueue queue;
    CommandRegistry registry;
    Keymap keymap;

    std::unique_ptr<platform::IAppLauncher> launcher; // outlives the workspace
    std::unique_ptr<ui::SessionWriter> writer;        // outlives nothing it reads (copies)
    std::unique_ptr<ui::Workspace> workspace;
    ui::WorkspaceView view;
    ui::SaveScheduler save;
    std::uint64_t seen_persist = 0;
    Path config_root;
    bool wayland = false;
    float content_scale = 1.0F; // X11: window size = logical size * content_scale
    bool text_input_on = false;
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
    void update_geometry();
    void apply_window_geometry(const layout::WindowGeometry& g, bool restore) const;
    void persist_tick(double t_ms);
    void final_save();
    void layout_intent(const ui::Intent& in);
    [[nodiscard]] layout::PaneId pane_at_index(std::size_t index) const;
    [[nodiscard]] layout::TabId tab_at(std::size_t pane, std::size_t tab) const;
    void dump(const std::string& file) const;
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

    // Session: decide where it lives and read it before the window exists (its size).
    if (!config.config_dir.empty()) {
        config_root = Path(std::string(config.config_dir));
    } else if (auto c = platform::user_dir(platform::UserDir::Config); c) {
        config_root = *c / layout::kConfigDirName;
    }
    const bool headless_default = config.max_frames > 0 && config.config_dir.empty();
    if (headless_default && (config.restore_session || config.save_session)) {
        SDL_Log("Navigator: --frames without --config-dir: session not read or written");
    }
    const bool restore = config.restore_session && !headless_default && !config_root.empty();
    const bool persist = config.save_session && !headless_default && !config_root.empty();
    std::optional<layout::Session> restored;
    if (restore) {
        Result<layout::Session> loaded = layout::load_session(layout::session_file(config_root));
        if (loaded) {
            restored = std::move(*loaded);
        } else if (loaded.error().code != ErrorCode::NotFound) {
            SDL_Log("Navigator: session not restored (%s): %s",
                    std::string(to_string(loaded.error().code)).c_str(),
                    loaded.error().message.c_str());
        }
    }
    int width = config.width;
    int height = config.height;
    const bool use_geometry = restored.has_value() && config.restore_window_geometry;
    if (use_geometry) {
        width = std::max(restored->window.width, kMinWindowWidth);
        height = std::max(restored->window.height, kMinWindowHeight);
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
    const char* video_driver = SDL_GetCurrentVideoDriver();
    wayland = video_driver != nullptr && std::string_view(video_driver) == "wayland";

    SDL_WindowFlags flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (use_geometry && restored->window.maximized) {
        flags |= SDL_WINDOW_MAXIMIZED;
    }
    window.reset(SDL_CreateWindow(title.c_str(), width, height, flags));
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
    content_scale = std::max(content, 1.0F);
    if (content > 1.01F) {
        SDL_SetWindowSize(window.get(),
                          static_cast<int>(std::lround(static_cast<float>(width) * content)),
                          static_cast<int>(std::lround(static_cast<float>(height) * content)));
    }
    SDL_SetWindowMinimumSize(window.get(), kMinWindowWidth, kMinWindowHeight);
    if (use_geometry) {
        apply_window_geometry(restored->window, true);
    }
    cursors.create();

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

    // Workspace: one BrowserModel per tab, restored from the session.
    if (auto h = platform::user_dir(platform::UserDir::Home); h) {
        home = *h;
    }
    if (auto l = platform::create_default_launcher(); l) {
        launcher = std::move(*l);
    } else {
        SDL_Log("Navigator: no app launcher (%s); files cannot be opened",
                l.error().message.c_str());
    }
    ui::Workspace::Options wo;
    wo.model.launcher = launcher.get();
    wo.model.watcher_factory = [] { return create_inotify_watcher(); };
    const Uint32 wake_type = wake_event;
    wo.model.wake = [wake_type] {
        if (wake_type == 0) {
            return;
        }
        SDL_Event e{};
        e.type = wake_type;
        SDL_PushEvent(&e); // thread-safe
    };
    wo.home = home;
    wo.row_dp = ui::density_metrics(density).row_height;
    wo.config_root = config_root;
    workspace = std::make_unique<ui::Workspace>(std::move(wo));
    const layout::WindowGeometry saved_window =
        restored ? restored->window : layout::WindowGeometry{};
    workspace->start(std::move(restored), Path(std::string(config.start_path)));
    workspace->window() = saved_window;
    update_geometry();
    if (persist) {
        writer = std::make_unique<ui::SessionWriter>(
            layout::session_file(config_root),
            [](const std::string& msg) { SDL_Log("Navigator: %s", msg.c_str()); });
    }
    (void)workspace->sync();
    seen_persist = workspace->persist_serial();
    return ok();
}

void App::Impl::apply_window_geometry(const layout::WindowGeometry& g, bool restore) const {
    if (!restore || wayland || !g.has_position || g.maximized) {
        return;
    }
    // Only a position that lands on a connected display (a monitor may be gone).
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    bool visible = false;
    for (int i = 0; displays != nullptr && i < count; ++i) {
        SDL_Rect b{};
        if (SDL_GetDisplayBounds(displays[i], &b) && g.x + 48 >= b.x && g.x + 48 < b.x + b.w &&
            g.y + 16 >= b.y && g.y + 16 < b.y + b.h) {
            visible = true;
        }
    }
    SDL_free(displays);
    if (visible) {
        SDL_SetWindowPosition(window.get(), g.x, g.y);
    }
}

void App::Impl::update_geometry() {
    if (!workspace || !window) {
        return;
    }
    layout::WindowGeometry g = workspace->window();
    const SDL_WindowFlags fl = SDL_GetWindowFlags(window.get());
    g.maximized = (fl & SDL_WINDOW_MAXIMIZED) != 0;
    if (!g.maximized && (fl & SDL_WINDOW_FULLSCREEN) == 0 && (fl & SDL_WINDOW_MINIMIZED) == 0) {
        int w = 0;
        int h = 0;
        if (SDL_GetWindowSize(window.get(), &w, &h) && w > 0 && h > 0) {
            g.width = static_cast<int>(std::lround(static_cast<float>(w) / content_scale));
            g.height = static_cast<int>(std::lround(static_cast<float>(h) / content_scale));
        }
        int x = 0;
        int y = 0;
        // Wayland clients cannot read or set their global position.
        if (!wayland && SDL_GetWindowPosition(window.get(), &x, &y)) {
            g.has_position = true;
            g.x = x;
            g.y = y;
        } else {
            g.has_position = false;
        }
    }
    if (!(g == workspace->window())) {
        workspace->window() = g;
        save.mark(now_ms());
    }
}

void App::Impl::persist_tick(double t_ms) {
    if (workspace->persist_serial() != seen_persist) {
        seen_persist = workspace->persist_serial();
        save.mark(t_ms);
    }
    if (save.due(t_ms)) {
        save.taken();
        if (writer) {
            writer->submit(workspace->snapshot());
        }
    }
}

void App::Impl::final_save() {
    if (!writer) {
        return;
    }
    (void)writer->flush();
    (void)workspace->sync();
    update_geometry();
    (void)writer->save_now(workspace->snapshot());
}

bool App::Impl::counting() const noexcept {
    // Scripted and screenshot runs count frames only once the browser has settled.
    if (script.empty() && screenshot_path.empty()) {
        return true;
    }
    return script_step >= script.size() && workspace->settled();
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
        // A popup's text field edits with Backspace (text editing, not a command).
        if (view.wants_text_input() && chord.key == Key::Backspace &&
            chord.mods == Modifiers::None) {
            view.backspace();
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
    case SDL_EVENT_TEXT_INPUT:
        if (event.text.text != nullptr) {
            view.text_input(event.text.text);
        }
        break;
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
        } else if (event.button.button == SDL_BUTTON_MIDDLE) {
            pointer.middle_pressed = true;
        } else if (event.button.button == SDL_BUTTON_RIGHT) {
            pointer.right_pressed = true;
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
        } else if (event.button.button == SDL_BUTTON_MIDDLE) {
            pointer.middle_released = true;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float y = event.wheel.y;
        float x = event.wheel.x;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            y = -y;
            x = -x;
        }
        pointer.wheel_y += y;
        pointer.wheel_x += x;
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: pointer.inside = false; break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER: pointer.inside = true; break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: focused = true; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        focused = false;
        view.cancel_drag();
        if (pointer.down) {
            pointer.down = false;
            pointer.released = true; // never leave a drag captured
        }
        break;
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED: update_geometry(); break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: update_scale(); break;
    default: break;
    }
}

void App::Impl::execute(const Command& command) {
    dirty = true;
    // An open popup takes the keyboard (docs/keymap.md, "Popups").
    if (view.popup_open() && view.route_command(command, queue, intents)) {
        return;
    }
    if (command.id == CommandId::ClearSelection && view.dragging()) {
        view.cancel_drag(); // Escape during a tab drag
        return;
    }
    switch (command.id) {
    case CommandId::None: return;
    case CommandId::Quit: running = false; return;
    case CommandId::ToggleFpsOverlay:
        show_fps = !show_fps;
        stats_reset = true;
        return;
    case CommandId::ToggleTheme:
        previous_theme = theme;
        theme = ui::toggled(theme);
        fade_start_ms = animations ? now_ms() : -1.0;
        return;
    case CommandId::SearchTabs:
        view.open_search_tabs(*workspace, home.empty() ? nullptr : &home);
        return;
    case CommandId::SaveLayout: view.open_save_layout(*workspace); return;
    case CommandId::LoadLayout: view.open_layout_picker(*workspace, false); return;
    case CommandId::DeleteLayout: view.open_layout_picker(*workspace, true); return;
    default: (void)workspace->execute(command); return;
    }
}

void App::Impl::layout_intent(const ui::Intent& in) {
    using Kind = ui::Intent::Kind;
    const auto report = [this](const Result<void>& r, std::string_view ok_key,
                               std::string_view fail_key, const std::string& name) {
        if (r) {
            view.show_toast(ui::substitute(tr(ok_key), "name", name), false, now_ms());
        } else {
            std::string msg = ui::substitute(tr(fail_key), "name", name);
            msg += " ";
            msg += error_text(r.error().code);
            view.show_toast(std::move(msg), true, now_ms());
            SDL_Log("Navigator: %s", r.error().message.c_str());
        }
    };
    switch (in.kind) {
    case Kind::SaveLayout:
        report(workspace->save_layout(in.text), "layout.saved", "layout.save_failed", in.text);
        break;
    case Kind::LoadLayout:
        report(workspace->load_layout(in.text), "layout.loaded", "layout.load_failed", in.text);
        break;
    case Kind::DeleteLayout: {
        const Result<void> r = workspace->delete_layout(in.text);
        report(r, "layout.deleted", "layout.delete_failed", in.text);
        if (r) {
            view.layout_deleted(in.text);
        }
        break;
    }
    default: break;
    }
}

void App::Impl::apply_intents() {
    using Kind = ui::Intent::Kind;
    // Intents recorded while applying (none today) wait for the next frame.
    std::vector<ui::Intent> batch;
    batch.swap(intents);
    for (const ui::Intent& in : batch) {
        dirty = true;
        ui::BrowserModel* m = workspace->model(layout::TabId{in.tab});
        switch (in.kind) {
        case Kind::ClickRow:
            if (m != nullptr) {
                m->click_row(in.row, in.mods);
            }
            break;
        case Kind::BandBegin:
            if (m != nullptr) {
                m->band_begin(in.additive);
            }
            break;
        case Kind::BandUpdate:
            if (m != nullptr) {
                m->band_update(in.row, in.row_last);
            }
            break;
        case Kind::BandEnd:
            if (m != nullptr) {
                m->band_end();
            }
            break;
        case Kind::ClearSelection:
            if (m != nullptr) {
                m->clear_selection();
            }
            break;
        case Kind::SortBy:
            if (m != nullptr) {
                m->sort_by(in.sort_key);
            }
            break;
        case Kind::NavigateTo:
            if (m != nullptr) {
                m->navigate_to(in.path);
            }
            break;
        case Kind::DismissNotice:
            if (m != nullptr) {
                m->dismiss_notice();
            }
            break;
        case Kind::CursorTo:
            if (m != nullptr) {
                m->cursor_to(in.row);
            }
            break;
        case Kind::ActivatePane: workspace->activate_pane(in.pane); break;
        case Kind::ActivateTab: workspace->activate_tab(layout::TabId{in.tab}); break;
        case Kind::DropTab: (void)workspace->drop(layout::TabId{in.tab}, in.drop); break;
        case Kind::MoveSplitter:
            workspace->move_splitter(in.split, in.index, in.position, in.min_fraction);
            break;
        case Kind::EqualizeSplit: workspace->equalize(in.split); break;
        case Kind::ReopenClosed: workspace->reopen_closed_at(in.index); break;
        case Kind::SaveLayout:
        case Kind::LoadLayout:
        case Kind::DeleteLayout: layout_intent(in); break;
        }
    }
}

layout::PaneId App::Impl::pane_at_index(std::size_t index) const {
    const std::vector<layout::PaneId> order = workspace->layout().pane_order();
    return index < order.size() ? order[index] : layout::PaneId{};
}

layout::TabId App::Impl::tab_at(std::size_t pane, std::size_t tab) const {
    const layout::PaneNode* p = workspace->layout().find_pane(pane_at_index(pane));
    return p != nullptr && tab < p->tabs.size() ? p->tabs[tab].id : layout::TabId{};
}

void App::Impl::dump(const std::string& file) const {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const layout::Layout& l = workspace->layout();
    const std::vector<layout::PaneId> order = l.pane_order();
    out << "panes " << order.size() << "\n";
    for (std::size_t i = 0; i < order.size(); ++i) {
        const layout::PaneNode* p = l.find_pane(order[i]);
        if (p == nullptr) {
            continue;
        }
        out << "pane " << i << (order[i] == l.active_pane() ? " active" : "") << " tabs "
            << p->tabs.size() << " active_tab " << p->active_tab << "\n";
        for (const layout::Tab& t : p->tabs) {
            const ui::BrowserModel* m = workspace->model(t.id);
            const Path& dir = m != nullptr && m->loaded() ? m->current_dir() : t.state.path;
            out << "  tab " << dir.string() << (m != nullptr && !m->suspended() ? " live" : "")
                << "\n";
        }
    }
    out << "closed " << workspace->closed_tabs().size() << "\n";
}

void App::Impl::run_script_step() {
    if (script_step >= script.size() || !workspace->settled()) {
        return;
    }
    const std::vector<std::string> parts = split(script[script_step], ':');
    const std::string raw = script[script_step];
    ++script_step;
    dirty = true;
    if (parts.empty()) {
        return;
    }
    const std::string& op = parts[0];
    const std::string arg = parts.size() > 1 ? parts[1] : std::string();
    const auto num = [&](std::size_t i) -> std::size_t {
        return i < parts.size()
                   ? static_cast<std::size_t>(std::strtoull(parts[i].c_str(), nullptr, 10))
                   : 0;
    };
    ui::BrowserModel& model = workspace->active_model();
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
        const std::size_t row = model.find(arg);
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
            model.click_row(row, mods);
        }
    } else if (op == "hover") {
        view.view(workspace->active_tab()).set_hover_override(model.find(arg));
    } else if (op == "go") {
        model.navigate_to(Path(raw.substr(3)));
    } else if (op == "blur") {
        focused = false;
    } else if (op == "type") {
        view.text_input(raw.substr(5));
    } else if (op == "focus") {
        workspace->activate_pane(pane_at_index(num(1)));
    } else if (op == "tab") {
        workspace->activate_tab(tab_at(num(1), num(2)));
    } else if (op == "hovertab") {
        view.debug_hover_tab(tab_at(num(1), num(2)));
    } else if (op == "dragpreview" && parts.size() >= 5) {
        ui::DropTarget t;
        t.pane = pane_at_index(num(3));
        const std::string& e = parts[4];
        t.kind = e == "centre" ? ui::DropKind::Centre : ui::DropKind::Edge;
        t.edge = e == "left"     ? layout::SplitEdge::Left
                 : e == "top"    ? layout::SplitEdge::Top
                 : e == "bottom" ? layout::SplitEdge::Bottom
                                 : layout::SplitEdge::Right;
        view.debug_drag(tab_at(num(1), num(2)), pane_at_index(num(1)), t);
    } else if (op == "dump") {
        dump(raw.substr(5));
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
    ui::ShellFrame frame{
        .painter = painter,
        .palette = pal,
        .theme = theme,
        .workspace = *workspace,
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
        .now_ms = t_ms,
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
    if (!workspace->settled()) {
        return;
    }
    const ui::BrowserModel& model = workspace->active_model();
    ui::BrowserView& list = view.view(workspace->active_tab());
    switch (bench_phase) {
    case 0:
        bench_phase = 1;
        list.scroll_by(-list.max_scroll());
        break;
    case 1:
        bench_jump.push_back(frame_ms);
        bench_build.push_back(build_ms);
        if (list.scroll() >= list.max_scroll() || bench_jump.size() >= 4000) {
            bench_phase = 2;
            list.scroll_by(-list.max_scroll());
        } else {
            list.scroll_by(static_cast<float>(list.page_rows()) *
                           ui::density_metrics(density).row_height);
        }
        break;
    case 2:
        bench_smooth.push_back(frame_ms);
        bench_build.push_back(build_ms);
        list.scroll_by(6.0F);
        if (bench_smooth.size() >= 600) {
            std::printf("bench-scroll: entries=%zu driver=%s scale=%.2f\n", model.entries().size(),
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
            Sint32 wait_ms = kIdleWaitMs;
            if (const std::optional<double> due = s.save.remaining(now_ms()); due.has_value()) {
                wait_ms = std::clamp(static_cast<Sint32>(std::ceil(*due)) + 1, 1, kIdleWaitMs);
            }
            if (SDL_WaitEventTimeout(&event, wait_ms)) {
                s.handle_event(event);
            }
        }
        if (s.workspace->frame_start()) {
            s.dirty = true;
        }
        while (SDL_PollEvent(&event)) {
            s.handle_event(event);
        }
        s.run_script_step();
        s.persist_tick(now_ms());
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
        for (const layout::TabId tab : s.workspace->visible_tabs()) {
            if (ui::BrowserModel* m = s.workspace->model(tab); m != nullptr) {
                if (const ui::BrowserView* v = s.view.find_view(tab); v != nullptr) {
                    m->set_page_rows(v->page_rows());
                    m->set_scroll(v->scroll());
                }
            }
        }
        s.queue.drain([&s](const Command& c) { s.execute(c); });
        s.view.prune(*s.workspace);
        (void)s.workspace->sync();
        s.persist_tick(now_ms());
        s.cursors.show(s.view.cursor());
        if (s.view.wants_text_input() != s.text_input_on) {
            s.text_input_on = s.view.wants_text_input();
            if (s.text_input_on) {
                SDL_StartTextInput(s.window.get());
            } else {
                SDL_StopTextInput(s.window.get());
            }
        }
        s.pointer.pressed = false;
        s.pointer.released = false;
        s.pointer.clicks = 0;
        s.pointer.wheel_y = 0.0F;
        s.pointer.wheel_x = 0.0F;
        s.pointer.middle_pressed = false;
        s.pointer.middle_released = false;
        s.pointer.right_pressed = false;

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
    s.final_save(); // quit, window close or max_frames: the session is written synchronously
    SDL_HideWindow(s.window.get());
    return 0;
}

CommandQueue& App::commands() noexcept { return impl_->queue; }

FrameStats App::frame_stats() const noexcept { return impl_->stats; }

Theme App::theme() const noexcept { return impl_->theme; }

} // namespace navigator
