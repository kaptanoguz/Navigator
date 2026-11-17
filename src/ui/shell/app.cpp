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
// Phase 4 (docs/architecture.md "Phase 4"): the sidebar (left of the pane tree; one per window)
// is drawn from rows rebuilt every frame from the PlacesModel; its volume and probe workers and
// the GoTo / address bar path worker deliver results at frame start (with the listings).
// Keyboard focus is the active pane, the sidebar, or a text field (popup query, GoTo, address
// bar, bookmark rename); fields take their editing keys before the keymap (apply_field_key),
// the sidebar and the address bar take the resolved list.* commands (docs/keymap.md "Focus
// scoping"). Bookmarks and recents follow the session's restore / save flags and headless rule.
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
//                                          Phase 4: key:CHORD (parse_key_chord, e.g. key:Tab,
//                                          key:Ctrl+A) through the same path as a key press;
//                                          sbhover:ROW / sbfocus[:ROW] (sidebar rows);
//                                          sbmenu:ROW (row context menu)
//   NAVIGATOR_DEBUG_VOLUMES=<dir>          fake volume table for the sidebar (debug_volumes.hpp)
//   NAVIGATOR_DEBUG_BENCH_SCROLL=1         scroll benchmark: logs frame times to stdout and quits
// User-facing: NAVIGATOR_NO_ANIMATIONS=1 forces AppConfig::animations off.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include <navigator/i18n.hpp>
#include <navigator/layout.hpp>
#include <navigator/path_input.hpp>
#include <navigator/places.hpp>
#include <navigator/platform.hpp>
#include <navigator/ui.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "browser/browser_view.hpp"
#include "i18n/catalog.hpp"
#include "input/sdl_input.hpp"
#include "path/copy_path.hpp"
#include "path/path_worker.hpp"
#include "render/backend_name.hpp"
#include "render/renderer.hpp"
#include "render/sdl_handles.hpp"
#include "shell/debug_volumes.hpp"
#include "shell/painter.hpp"
#include "shell/ui_settings.hpp"
#include "sidebar/places_model.hpp"
#include "sidebar/sidebar_model.hpp"
#include "sidebar/sidebar_view.hpp"
#include "text/font_cache.hpp"
#include "text/text_renderer.hpp"
#include "theme/theme.hpp"
#include "widgets/text_edit.hpp"
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
    std::unique_ptr<ui::PathWorker> path_worker;      // GoTo / address bar jobs
    std::unique_ptr<ui::SessionWriter> writer;        // outlives nothing it reads (copies)
    std::unique_ptr<ui::Workspace> workspace;
    ui::WorkspaceView view;
    // Phase 4: sidebar data (stores, volume / probe workers, persistence) and view.
    std::unique_ptr<ui::PlacesModel> places;
    ui::SidebarView sidebar;
    std::vector<ui::SidebarRow> sidebar_rows;
    std::uint64_t rows_serial = 0; // inputs of sidebar_rows (rebuilt when one changes)
    layout::SidebarState rows_state;
    Path rows_folder;
    Language rows_language = Language::English;
    ui::RectF sidebar_rect;
    bool sidebar_icon_only = false;
    bool sidebar_focus = false; // keyboard focus is in the sidebar
    std::size_t sidebar_row = static_cast<std::size_t>(-1);
    std::vector<ui::PathOutcome> outcomes;
    places::BookmarkId focus_bookmark; // move keyboard focus to this bookmark's row
    bool focus_on_show = false;        // sidebar.focus showed the sidebar: focus it next frame
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
    void handle_key(const KeyChord& chord, bool repeat);
    [[nodiscard]] ui::TextEdit* text_field() noexcept;
    void field_edited();
    bool route_sidebar(const Command& command);
    void open_sidebar_row(std::size_t row, bool new_tab);
    void sidebar_menu(std::size_t row, float x, float y);
    void commit_rename();
    void focus_sidebar(bool on);
    void layout_sidebar(float width_px, float height_px);
    void apply_outcomes();
    void bookmark_command(const Command& command);
    [[nodiscard]] places::BookmarkId target_bookmark(std::uint64_t arg) const;
    [[nodiscard]] std::vector<ui::GotoSource> goto_sources() const;
    void copy_path(std::uint64_t arg);
    void set_clipboard(const std::string& value) const;
    void toast(std::string message, bool error = false);
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
    const std::function<void()> wake_fn = wo.model.wake;
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

    // Phase 4: GoTo / address bar worker, places (bookmarks, recents, volumes).
    path_worker = std::make_unique<ui::PathWorker>(wake_fn);
    view.attach_paths(path_worker.get());
    ui::PlacesModel::Options po;
    po.home = home;
    if (!home.empty()) {
        // The XDG user-dirs file (small; read once like the session).
        Path xdg_config = home / ".config";
        if (auto c = platform::user_dir(platform::UserDir::Config); c) {
            xdg_config = *c;
        }
        if (Result<platform::UserDirs> d = platform::read_user_dirs(home, xdg_config); d) {
            po.user_dirs = std::move(*d);
        } else {
            SDL_Log("Navigator: user dirs not read: %s", d.error().message.c_str());
        }
    }
    po.config_root = config_root;
    po.restore = restore;
    po.persist = persist;
    po.volume_worker = true;
    po.volume_query.home_dir = home;
    if (const char* dv = env("NAVIGATOR_DEBUG_VOLUMES")) {
        if (Result<VolumeQuery> q = ui::debug_volume_query(Path(dv), home); q) {
            po.volume_query = std::move(*q);
        } else {
            SDL_Log("Navigator: %s", q.error().message.c_str());
        }
    }
    po.wake = wake_fn;
    po.log = [](const std::string& msg) { SDL_Log("Navigator: %s", msg.c_str()); };
    places = std::make_unique<ui::PlacesModel>(std::move(po));
    places->load();
    places->set_sidebar_visible(workspace->sidebar().visible);
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
    if (places) {
        places->tick(t_ms);
    }
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
    if (places) {
        places->final_save();
    }
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
    return script_step >= script.size() && workspace->settled() && !view.paths_busy() &&
           (!places || (places->volumes_ready() && !places->probe_pending()));
}

bool App::Impl::continuous() const noexcept {
    return max_frames > 0 || show_fps || fade_start_ms >= 0.0 || view.animating() || bench ||
           sidebar.animating();
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
            // The Menu key has no Key value; it opens the active tab's context menu.
            if ((event.key.key == SDLK_APPLICATION || event.key.key == SDLK_MENU) &&
                !event.key.repeat && !view.popup_open()) {
                view.request_tab_menu();
            }
            break;
        }
        handle_key(chord, event.key.repeat);
        break;
    }
    case SDL_EVENT_KEY_UP: pointer.mods = ui::translate_modifiers(event.key.mod); break;
    case SDL_EVENT_TEXT_INPUT:
        if (event.text.text != nullptr) {
            if (sidebar.renaming() && !view.popup_open()) {
                (void)sidebar.rename_edit()->insert(event.text.text);
            } else {
                view.text_input(event.text.text);
            }
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

void App::Impl::handle_key(const KeyChord& chord, bool repeat) {
    // Delete in the layout picker deletes the highlighted layout (popup-local, docs/keymap.md
    // "Popups"), before its filter field sees the key.
    if (chord == KeyChord{Key::Delete, Modifiers::None} && !repeat && view.delete_key(intents)) {
        return;
    }
    // Focus scoping step 1: a focused text field takes its editing keys first.
    if (ui::TextEdit* field = text_field(); field != nullptr) {
        const ui::FieldKeyResult r = ui::apply_field_key(*field, chord, [] {
            std::string out;
            if (char* t = SDL_GetClipboardText(); t != nullptr) {
                out = t;
                SDL_free(t);
            }
            return out;
        });
        if (r.handled) {
            dirty = true;
            if (r.clipboard) {
                set_clipboard(*r.clipboard);
            }
            if (r.tab) {
                if (!sidebar.renaming()) {
                    (void)view.field_tab(now_ms());
                }
            } else if (r.changed) {
                field_edited();
            }
            return;
        }
    }
    if (const auto cmd = resolve_command(keymap, registry, chord); cmd) {
        // Auto-repeat moves the cursor; it never re-fires toggles or navigation.
        if (!repeat || is_cursor_command(cmd->id)) {
            queue.enqueue(*cmd);
        }
    } else if (!repeat && !view.popup_open() && chord == KeyChord{Key::F10, Modifiers::Shift}) {
        // Last-resort fallback until a registered command exists: the platform convention for
        // a context menu (Shift+F10) on the active tab.
        view.request_tab_menu();
    }
}

ui::TextEdit* App::Impl::text_field() noexcept {
    if (sidebar.renaming() && !view.popup_open()) {
        return sidebar.rename_edit();
    }
    return view.active_field();
}

void App::Impl::field_edited() {
    if (sidebar.renaming() && !view.popup_open()) {
        return;
    }
    view.field_changed(now_ms());
}

void App::Impl::set_clipboard(const std::string& value) const {
    if (!SDL_SetClipboardText(value.c_str())) {
        SDL_Log("Navigator: clipboard unavailable: %s", SDL_GetError());
    }
}

void App::Impl::toast(std::string message, bool error) {
    view.show_toast(std::move(message), error, now_ms());
}

void App::Impl::focus_sidebar(bool on) {
    if (on) {
        if (!workspace->sidebar().visible) {
            return;
        }
        view.close_address_bar();
        if (sidebar_rows.empty()) {
            focus_on_show = true; // no rows before the first frame: focus once they exist
            return;
        }
        if (sidebar_row >= sidebar_rows.size() || !sidebar_rows[sidebar_row].focusable()) {
            sidebar_row = ui::sidebar_initial_focus(sidebar_rows);
        }
        sidebar.reveal(sidebar_row);
    } else {
        sidebar.end_rename();
    }
    sidebar_focus = on;
}

bool App::Impl::route_sidebar(const Command& command) {
    constexpr auto npos = static_cast<std::size_t>(-1);
    const auto step = [this](std::size_t from, std::ptrdiff_t delta) {
        const std::size_t next = ui::sidebar_focus_step(sidebar_rows, from, delta);
        if (next != npos) {
            sidebar_row = next;
            sidebar.reveal(next);
        }
    };
    switch (command.id) {
    case CommandId::CursorUp: step(sidebar_row, -1); return true;
    case CommandId::CursorDown: step(sidebar_row, 1); return true;
    case CommandId::CursorPageUp: step(sidebar_row, -8); return true;
    case CommandId::CursorPageDown: step(sidebar_row, 8); return true;
    case CommandId::CursorHome: step(npos, 1); return true;
    case CommandId::CursorEnd: step(npos, -1); return true;
    case CommandId::Activate:
        if (sidebar_row < sidebar_rows.size()) {
            const ui::SidebarRow row = sidebar_rows[sidebar_row];
            open_sidebar_row(sidebar_row, false);
            if (row.item() && row.online()) {
                focus_sidebar(false); // the place opened: keep working in the pane
            }
        }
        return true;
    case CommandId::OpenInNewTab:
        if (sidebar_row < sidebar_rows.size()) {
            open_sidebar_row(sidebar_row, true);
        }
        return true;
    case CommandId::ClearSelection: focus_sidebar(false); return true;
    case CommandId::SelectAll: return true; // nothing to select in the sidebar
    default: return false;
    }
}

void App::Impl::open_sidebar_row(std::size_t row, bool new_tab) {
    if (row >= sidebar_rows.size()) {
        return;
    }
    const ui::SidebarRow& r = sidebar_rows[row];
    if (r.kind == ui::SidebarRowKind::Header) {
        layout::SidebarState st = workspace->sidebar();
        st.set_collapsed(r.section, !st.is_collapsed(r.section));
        workspace->set_sidebar(st);
        return;
    }
    if (!r.item()) {
        return;
    }
    if (r.status == places::LocationStatus::Offline) {
        toast(ui::substitute(tr("sidebar.offline_notice"), "name", r.label), true);
        return;
    }
    if (r.status == places::LocationStatus::Missing) {
        toast(ui::substitute(tr("sidebar.missing_notice"), "name", r.label), true);
        return;
    }
    view.close_address_bar();
    if (new_tab) {
        (void)workspace->open_tab(r.path, false);
    } else {
        workspace->navigate_active(r.path);
    }
}

void App::Impl::sidebar_menu(std::size_t row, float x, float y) {
    if (row >= sidebar_rows.size()) {
        return;
    }
    const ui::SidebarRow& r = sidebar_rows[row];
    using Entry = ui::WorkspaceView::MenuEntry;
    std::vector<Entry> entries;
    const auto cmd_entry = [&](std::string_view key, CommandId id, std::uint64_t arg,
                               std::string_view hint, bool enabled, bool sep) {
        Entry e;
        e.label = std::string(tr(key));
        e.command = Command{id, arg};
        e.hint_command = std::string(hint);
        e.enabled = enabled;
        e.separator_before = sep;
        entries.push_back(std::move(e));
    };
    if (r.item()) {
        const bool online = r.online();
        const auto open_entry = [&](std::string_view key, bool new_tab, std::string_view hint) {
            Entry e;
            e.label = std::string(tr(key));
            ui::Intent in;
            in.kind = ui::Intent::Kind::OpenPath;
            in.path = r.path;
            in.additive = new_tab;
            e.intent = in;
            e.enabled = online;
            e.hint_command = std::string(hint);
            entries.push_back(std::move(e));
        };
        open_entry("menu.open", false, "");
        open_entry("menu.open_in_new_tab", true, "list.open_in_new_tab");
        Entry copy;
        copy.label = std::string(tr("menu.copy_path"));
        ui::Intent in;
        in.kind = ui::Intent::Kind::CopyText;
        in.text = ui::copy_path_text(std::vector<Path>{r.path});
        copy.intent = in;
        entries.push_back(std::move(copy));
    }
    if (r.kind == ui::SidebarRowKind::Bookmark) {
        const std::size_t at = ui::sidebar_bookmark_index(sidebar_rows, row);
        const std::uint64_t id = r.bookmark.value;
        cmd_entry("menu.rename", CommandId::RenameBookmark, id, "bookmarks.rename", true, true);
        cmd_entry("menu.move_up", CommandId::MoveBookmarkUp, id, "bookmarks.move_up", at > 0,
                  false);
        cmd_entry("menu.move_down", CommandId::MoveBookmarkDown, id, "bookmarks.move_down",
                  at + 1 < places->bookmarks().size(), false);
        cmd_entry("menu.remove_bookmark", CommandId::ToggleBookmark, id, "", true, true);
    }
    if (r.section == layout::SidebarSection::Recents &&
        (r.kind == ui::SidebarRowKind::Recent || r.kind == ui::SidebarRowKind::Header)) {
        cmd_entry("menu.clear_recents", CommandId::ClearRecents, 0, "recents.clear",
                  !places->recents().empty(), r.item());
        cmd_entry(places->recents().options().enabled ? "menu.stop_tracking"
                                                      : "menu.resume_tracking",
                  CommandId::ToggleRecentsTracking, 0, "recents.toggle_tracking", true, false);
    }
    if (r.kind == ui::SidebarRowKind::Header && r.section == layout::SidebarSection::Bookmarks) {
        cmd_entry("menu.bookmark_folder", CommandId::ToggleBookmark, 0, "bookmarks.toggle", true,
                  false);
    }
    if (!entries.empty()) {
        view.open_menu(keymap, std::move(entries), x, y);
    }
}

places::BookmarkId App::Impl::target_bookmark(std::uint64_t arg) const {
    if (arg != 0) {
        if (arg > 0xFFFFFFFFULL) {
            return {};
        }
        const places::BookmarkId id{static_cast<std::uint32_t>(arg)};
        return places->bookmarks().find(id) != nullptr ? id : places::BookmarkId{};
    }
    if (sidebar_focus && sidebar_row < sidebar_rows.size() &&
        sidebar_rows[sidebar_row].kind == ui::SidebarRowKind::Bookmark) {
        return sidebar_rows[sidebar_row].bookmark;
    }
    return {};
}

void App::Impl::bookmark_command(const Command& command) {
    const auto now =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    switch (command.id) {
    case CommandId::ToggleBookmark: {
        if (command.arg != 0) {
            const places::BookmarkId id = target_bookmark(command.arg);
            if (const places::Bookmark* b = places->bookmarks().find(id); b != nullptr) {
                const std::string name = b->name.empty() ? places::default_label(b->path) : b->name;
                if (places->remove_bookmark(id)) {
                    toast(ui::substitute(tr("bookmarks.removed"), "name", name));
                }
            }
            return;
        }
        const ui::BrowserModel& m = workspace->active_model();
        if (!m.loaded() || m.current_dir().empty()) {
            return;
        }
        const std::string name = places::default_label(m.current_dir());
        switch (places->toggle_bookmark(m.current_dir(), now)) {
        case ui::PlacesModel::Toggled::Added:
            toast(ui::substitute(tr("bookmarks.added"), "name", name));
            break;
        case ui::PlacesModel::Toggled::Removed:
            toast(ui::substitute(tr("bookmarks.removed"), "name", name));
            break;
        case ui::PlacesModel::Toggled::Failed:
            toast(ui::substitute(tr("bookmarks.add_failed"), "name", name), true);
            break;
        }
        return;
    }
    case CommandId::RenameBookmark: {
        const places::BookmarkId id = target_bookmark(command.arg);
        const places::Bookmark* b = places->bookmarks().find(id);
        if (b == nullptr) {
            return;
        }
        layout::SidebarState st = workspace->sidebar();
        st.visible = true;
        st.set_collapsed(layout::SidebarSection::Bookmarks, false);
        workspace->set_sidebar(st);
        places->set_sidebar_visible(true);
        sidebar.begin_rename(id, b->name.empty() ? places::default_label(b->path) : b->name);
        sidebar_focus = true;
        focus_bookmark = id;
        return;
    }
    case CommandId::MoveBookmarkUp:
    case CommandId::MoveBookmarkDown: {
        const places::BookmarkId id = target_bookmark(command.arg);
        if (id.valid() &&
            places->move_bookmark(id, command.id == CommandId::MoveBookmarkUp ? -1 : 1) &&
            sidebar_focus) {
            focus_bookmark = id; // keep keyboard focus on the moved row
        }
        return;
    }
    default: return;
    }
}

void App::Impl::commit_rename() {
    const places::BookmarkId id = sidebar.rename_target();
    ui::TextEdit* edit = sidebar.rename_edit();
    if (edit != nullptr && id.valid()) {
        if (Result<void> r = places->rename_bookmark(id, edit->text()); !r) {
            toast(std::string(tr("bookmarks.rename_failed")), true);
        }
    }
    sidebar.end_rename();
}

std::vector<ui::GotoSource> App::Impl::goto_sources() const {
    std::vector<ui::GotoSource> out;
    std::size_t recents = 0;
    for (const places::RecentEntry& e : places->recents().entries()) {
        if (recents++ >= 20) {
            break;
        }
        ui::GotoSource g;
        g.kind = ui::GotoSource::Kind::Recent;
        g.status = places->status(e.path, e.volume, &g.path);
        g.label = places::default_label(e.path);
        out.push_back(std::move(g));
    }
    for (const places::Bookmark& b : places->bookmarks().list()) {
        ui::GotoSource g;
        g.kind = ui::GotoSource::Kind::Bookmark;
        g.status = places->status(b.path, b.volume, &g.path);
        g.label = b.name.empty() ? places::default_label(b.path) : b.name;
        out.push_back(std::move(g));
    }
    for (const places::Place& pl : places->places()) {
        if (places->missing(pl.path)) {
            continue;
        }
        ui::GotoSource g;
        g.kind = ui::GotoSource::Kind::Place;
        g.place = pl.kind;
        g.path = pl.path;
        g.label = std::string(tr(places::label_key(pl.kind)));
        out.push_back(std::move(g));
    }
    return out;
}

void App::Impl::copy_path(std::uint64_t arg) {
    const ui::BrowserModel& m = workspace->active_model();
    const std::vector<Path> paths =
        ui::copy_path_targets(m, (arg & CopyPathArg::CurrentFolder) != 0U);
    if (paths.empty()) {
        return;
    }
    const std::string copied = ui::copy_path_text(paths);
    set_clipboard(copied);
    if (paths.size() == 1) {
        toast(ui::substitute(tr("copy.copied_one"), "path", copied));
    } else {
        toast(ui::tr_count("copy.copied", paths.size()));
    }
}

void App::Impl::apply_outcomes() {
    std::vector<ui::PathOutcome> batch;
    batch.swap(outcomes);
    for (const ui::PathOutcome& o : batch) {
        dirty = true;
        switch (o.kind) {
        case ui::PathOutcome::Kind::Navigate:
        case ui::PathOutcome::Kind::RevealFile:
            if (o.new_tab) {
                (void)workspace->open_tab(o.path, false);
            } else {
                workspace->navigate_active(o.path, o.cursor_name);
            }
            break;
        case ui::PathOutcome::Kind::OpenFile: {
            const Result<void> opened =
                launcher ? platform::open_with_default(o.path, *launcher)
                         : Result<void>(fail(ErrorCode::Unsupported, "no launcher"));
            if (!opened) {
                toast(ui::substitute(tr("notice.open_file_failed"), "name",
                                     places::default_label(o.path)),
                      true);
            }
            break;
        }
        }
    }
}

void App::Impl::layout_sidebar(float width_px, float height_px) {
    const layout::SidebarState& st = workspace->sidebar();
    sidebar_icon_only = ui::sidebar_icon_only(width_px / scale);
    if (!st.visible) {
        sidebar_rect = ui::RectF{};
        sidebar_rows.clear();
        sidebar_focus = false;
        rows_serial = 0;
        return;
    }
    float w_dp = sidebar_icon_only
                     ? ui::SidebarView::kIconOnlyWidth
                     : static_cast<float>(std::clamp(st.width, layout::kSidebarMinWidth,
                                                     layout::kSidebarMaxWidth));
    // Never squeeze the panes below one minimum pane width.
    w_dp = std::min(w_dp, std::max((width_px / scale) - ui::WorkspaceView::kMinPaneWidth,
                                   ui::SidebarView::kIconOnlyWidth));
    sidebar_rect = ui::RectF{0.0F, 0.0F, std::round(w_dp * scale), height_px};
    const ui::BrowserModel* m = workspace->model(workspace->active_tab());
    const Path folder = m != nullptr && m->loaded() ? m->current_dir() : Path();
    // Rows change only with the places data, the sidebar state, the folder or the language.
    if (places->serial() != rows_serial || !(st == rows_state) || folder != rows_folder ||
        language() != rows_language || sidebar_rows.empty()) {
        sidebar_rows = ui::build_sidebar_rows(*places, st, folder);
        rows_serial = places->serial();
        rows_state = st;
        rows_folder = folder;
        rows_language = language();
    }
    if (focus_on_show) {
        focus_on_show = false;
        if (sidebar_row >= sidebar_rows.size() || !sidebar_rows[sidebar_row].focusable()) {
            sidebar_row = ui::sidebar_initial_focus(sidebar_rows);
        }
        sidebar_focus = true;
        sidebar.reveal(sidebar_row);
    }
    if (focus_bookmark.valid()) {
        if (const std::size_t r = ui::sidebar_row_of(sidebar_rows, focus_bookmark);
            r < sidebar_rows.size()) {
            sidebar_row = r;
            sidebar.reveal(r);
        }
        focus_bookmark = places::BookmarkId{};
    }
    if (sidebar_focus && sidebar_row >= sidebar_rows.size()) {
        sidebar_row = ui::sidebar_initial_focus(sidebar_rows);
    } else if (sidebar_focus && !sidebar_rows[sidebar_row].focusable()) {
        sidebar_row = ui::sidebar_focus_step(sidebar_rows,
                                             std::min(sidebar_row, sidebar_rows.size() - 1), -1);
        if (sidebar_row >= sidebar_rows.size()) {
            sidebar_row = ui::sidebar_initial_focus(sidebar_rows);
        }
    }
}

void App::Impl::execute(const Command& command) {
    dirty = true;
    // An open popup takes the keyboard (docs/keymap.md, "Popups").
    if (view.popup_open() && view.route_command(command, queue, intents)) {
        return;
    }
    // The bookmark rename field: Enter commits, Escape cancels.
    if (sidebar.renaming()) {
        if (command.id == CommandId::Activate) {
            commit_rename();
            return;
        }
        if (command.id == CommandId::ClearSelection) {
            sidebar.end_rename();
            return;
        }
    }
    // Focus scoping step 2 (docs/keymap.md): the address bar and the sidebar take list.*.
    if (view.route_address(command)) {
        return;
    }
    if (sidebar_focus && route_sidebar(command)) {
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
    // Phase 4.
    case CommandId::ToggleSidebar: {
        layout::SidebarState st = workspace->sidebar();
        st.visible = !st.visible;
        workspace->set_sidebar(st);
        places->set_sidebar_visible(st.visible);
        if (!st.visible) {
            focus_sidebar(false);
        }
        return;
    }
    case CommandId::ToggleViewLock: return; // stub (docs/keymap.md)
    case CommandId::FocusAddressBar:
        focus_sidebar(false);
        view.open_address_bar(*workspace, home, now_ms());
        return;
    case CommandId::GoTo:
        focus_sidebar(false);
        view.close_address_bar();
        view.open_goto(*workspace, home, goto_sources(), now_ms());
        return;
    case CommandId::FocusSidebar:
        if (!workspace->sidebar().visible) {
            layout::SidebarState st = workspace->sidebar();
            st.visible = true;
            workspace->set_sidebar(st);
            places->set_sidebar_visible(true);
            focus_on_show = true; // its rows exist from the next frame
            return;
        }
        focus_sidebar(true);
        return;
    case CommandId::ToggleBookmark:
    case CommandId::RenameBookmark:
    case CommandId::MoveBookmarkUp:
    case CommandId::MoveBookmarkDown: bookmark_command(command); return;
    case CommandId::ClearRecents: places->clear_recents(now_ms()); return;
    case CommandId::ToggleRecentsTracking:
        toast(std::string(tr(places->toggle_tracking(now_ms()) ? "recents.tracking_on"
                                                               : "recents.tracking_off")));
        return;
    case CommandId::CopyPath: copy_path(command.arg); return;
    case CommandId::FocusNextPane:
    case CommandId::FocusPreviousPane: {
        if (!workspace->sidebar().visible) {
            sidebar_focus = false;
            (void)workspace->execute(command);
            return;
        }
        const std::vector<layout::PaneId> order = workspace->layout().pane_order();
        std::size_t at = 0;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i] == workspace->active_pane()) {
                at = i;
            }
        }
        const ui::FocusStop stop = ui::cycle_focus(order.size(), true, sidebar_focus, at,
                                                   command.id == CommandId::FocusNextPane ? 1 : -1);
        if (stop.sidebar) {
            focus_sidebar(true);
        } else {
            focus_sidebar(false);
            if (stop.pane < order.size()) {
                workspace->activate_pane(order[stop.pane]);
            }
        }
        return;
    }
    default:
        if (command.id == CommandId::NavigateBack || command.id == CommandId::NavigateForward ||
            command.id == CommandId::NavigateUp) {
            view.close_address_bar();
        }
        (void)workspace->execute(command);
        return;
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
        case Kind::SidebarOpen: open_sidebar_row(in.row, in.additive); break;
        case Kind::SidebarToggleSection: {
            layout::SidebarState st = workspace->sidebar();
            const auto sec = static_cast<layout::SidebarSection>(
                std::min<std::size_t>(in.index, layout::kSidebarSectionCount - 1));
            st.set_collapsed(sec, !st.is_collapsed(sec));
            workspace->set_sidebar(st);
            break;
        }
        case Kind::SidebarResize: {
            layout::SidebarState st = workspace->sidebar();
            st.width = std::round(in.position);
            workspace->set_sidebar(st);
            break;
        }
        case Kind::SidebarMenu: sidebar_menu(in.row, in.x, in.y); break;
        case Kind::SidebarFocusRow:
            sidebar_row = in.row;
            focus_sidebar(true);
            break;
        case Kind::RenameBookmark:
            (void)places->rename_bookmark(places::BookmarkId{static_cast<std::uint32_t>(in.index)},
                                          in.text);
            break;
        case Kind::MoveBookmark:
            (void)places->move_bookmark_to(places::BookmarkId{static_cast<std::uint32_t>(in.index)},
                                           in.row);
            break;
        case Kind::BookmarkTab: {
            const ui::BrowserModel* tm = workspace->model(layout::TabId{in.tab});
            if (tm != nullptr && tm->loaded()) {
                const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now());
                const std::string name = places::default_label(tm->current_dir());
                if (places->add_bookmark(tm->current_dir(), now, in.row)) {
                    toast(ui::substitute(tr("bookmarks.added"), "name", name));
                } else if (places->bookmarks().contains(tm->current_dir())) {
                    toast(ui::substitute(tr("bookmarks.exists"), "name", name));
                }
            }
            break;
        }
        case Kind::OpenPath:
            view.close_address_bar();
            if (in.additive) {
                (void)workspace->open_tab(in.path, false);
            } else {
                workspace->navigate_active(in.path);
            }
            break;
        case Kind::CopyText:
            set_clipboard(in.text);
            toast(ui::substitute(tr("copy.copied_one"), "path", in.text));
            break;
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
    if (script_step >= script.size() || !workspace->settled() || view.paths_busy() ||
        !outcomes.empty() || !queue.empty()) {
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
        if (sidebar.renaming() && !view.popup_open()) {
            (void)sidebar.rename_edit()->insert(raw.substr(5));
        } else {
            view.text_input(raw.substr(5));
        }
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
    } else if (op == "key") {
        if (Result<KeyChord> chord = parse_key_chord(raw.substr(4)); chord) {
            handle_key(*chord, false);
        } else {
            SDL_Log("Navigator: script: bad chord '%s'", raw.substr(4).c_str());
        }
    } else if (op == "sbhover") {
        sidebar.set_hover_override(num(1));
    } else if (op == "sbfocus") {
        if (parts.size() > 1) {
            sidebar_row = num(1);
        }
        focus_sidebar(true);
    } else if (op == "sbmenu") {
        if (const std::optional<ui::RectF> r = sidebar.row_rect(num(1)); r) {
            sidebar_menu(num(1), r->x + (0.6F * r->w), r->center_y());
        }
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
        sidebar.clear_caches();
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
        .content = {},
        .bookmarks = &places->bookmarks(),
        .sidebar_visible = false,
        .draw_sidebar = {},
        .draw_sidebar_overlay = {},
    };
    // Phase 4: the sidebar takes the left of the window; the panes get the rest.
    layout_sidebar(static_cast<float>(w), static_cast<float>(h));
    const bool sidebar_shown = sidebar_rect.w > 0.0F;
    frame.content = ui::RectF{sidebar_rect.w, 0.0F, static_cast<float>(w) - sidebar_rect.w,
                              static_cast<float>(h)};
    // The theme switch lives in the full sidebar's top bar; icon-only leaves it in the strip.
    frame.sidebar_visible = sidebar_shown && !sidebar_icon_only;
    ui::PointerInput masked;
    masked.x = -1.0e6F;
    masked.y = -1.0e6F;
    masked.released = pointer.released;
    masked.mods = pointer.mods;
    const bool popup = view.popup_open();
    const ui::SidebarFrame sf{
        .painter = painter,
        .palette = pal,
        .theme = theme,
        .rows = sidebar_rows,
        .keymap = keymap,
        .pointer = popup ? masked : pointer,
        .queue = queue,
        .intents = intents,
        .rect = sidebar_rect,
        .icon_only = sidebar_icon_only,
        .focused = sidebar_focus && focused,
        .focus_row = sidebar_row,
        .window_focused = focused,
        .density = density,
        .dragged_tab = view.dragged_tab(),
        .popup_open = popup,
    };
    if (sidebar_shown) {
        frame.draw_sidebar = [this, &sf] { sidebar.draw(sf); };
        frame.draw_sidebar_overlay = [this, &sf] { sidebar.draw_overlay(sf); };
    }
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
            for (const std::optional<double> due :
                 {s.save.remaining(now_ms()), s.places->save_remaining(now_ms())}) {
                if (due.has_value()) {
                    wait_ms = std::min(wait_ms, std::clamp(static_cast<Sint32>(std::ceil(*due)) + 1,
                                                           1, kIdleWaitMs));
                }
            }
            if (SDL_WaitEventTimeout(&event, wait_ms)) {
                s.handle_event(event);
            }
        }
        if (s.workspace->frame_start()) {
            s.dirty = true;
        }
        if (s.places->frame_start()) {
            s.dirty = true;
        }
        if (std::vector<Path> visits = s.workspace->take_visits(); !visits.empty()) {
            const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now());
            for (const Path& v : visits) {
                s.places->record_visit(v, now, now_ms());
            }
            s.dirty = true;
        }
        if (s.view.poll_paths(now_ms(), s.outcomes)) {
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
        // A press outside the sidebar moves keyboard focus back to the panes.
        if (s.pointer.pressed && s.sidebar_focus &&
            !contains(s.sidebar_rect, s.pointer.x, s.pointer.y) && !s.view.popup_open()) {
            s.focus_sidebar(false);
        }
        s.apply_intents();
        s.apply_outcomes();
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
        s.cursors.show(s.sidebar.resize_cursor() ? ui::PointerCursor::ResizeHorizontal
                                                 : s.view.cursor());
        if ((s.view.wants_text_input() || s.sidebar.renaming()) != s.text_input_on) {
            s.text_input_on = s.view.wants_text_input() || s.sidebar.renaming();
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
