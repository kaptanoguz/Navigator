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
// Phase 5 (docs/architecture.md "Phase 5"): each pane's filter field is the fourth keyboard
// owner (after popups, the address bar / rename field and the sidebar): it takes only its
// editing keys (search/filter_rules.hpp) and everything else reaches the pane through the
// keymap; SDL text input stays on so typing in the list starts the filter (type-to-filter).
// The name-index service starts after the first frame and stops (bounded) at quit; it never
// runs, reads or writes the cache in headless runs without --cache-dir, or with --no-index.
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
//                                          Phase 5: filter:TEXT (focus + set the filter),
//                                          waitsearch (hold until no visible search runs),
//                                          waitfound:N (hold until a flatten found N),
//                                          waitindex (hold until the folder's index is ready),
//                                          blurfilter
//   NAVIGATOR_DEBUG_SLOW_WALK=<ms>         flatten walks deliver small batches this slowly
//   NAVIGATOR_DEBUG_INDEX_ROOT=<dir>       index only <dir>, as if it were a volume
//   NAVIGATOR_DEBUG_BENCH_FLATTEN=1        flatten acceptance: flatten, cancel, report, quit
//   NAVIGATOR_DEBUG_VOLUMES=<dir>          fake volume table for the sidebar (debug_volumes.hpp)
//   NAVIGATOR_DEBUG_BENCH_SCROLL=1         scroll benchmark: logs frame times to stdout and quits
//   NAVIGATOR_DEBUG_OPS_DELAY_MS=<ms>      every file operation waits this long per entry
//                                          (screenshots of a running copy; fixtures only)
//   Phase 7 script steps: waitops (hold until no operation runs), waitprogress (hold until
//   one has copied bytes), waitconflict (hold until a conflict sheet is open),
//   conflict:skip|replace|keep|merge|cancel[:all], confirm:yes|no, panel (open the operations
//   panel), trashclick:NAME, dragstart:NAME, dragover:row:NAME | sidebar:LABEL | pane:N |
//   trash, dragend (drop where it is), dragcancel, rename:NAME (open the inline editor)
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
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <unistd.h>

#include <navigator/appearance.hpp>
#include <navigator/archive.hpp>
#include <navigator/batch_rename.hpp>
#include <navigator/git.hpp>
#include <navigator/i18n.hpp>
#include <navigator/keymap_file.hpp>
#include <navigator/layout.hpp>
#include <navigator/menu.hpp>
#include <navigator/openwith.hpp>
#include <navigator/palette.hpp>
#include <navigator/path_input.hpp>
#include <navigator/places.hpp>
#include <navigator/platform.hpp>
#include <navigator/plugins.hpp>
#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/transfer.hpp>
#include <navigator/trash.hpp>
#include <navigator/ui.hpp>
#include <navigator/view_settings.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "browser/browser_view.hpp"
#include "dnd/file_drag.hpp"
#include "i18n/catalog.hpp"
#include "input/sdl_input.hpp"
#include "input/shortcut_text.hpp"
#include "inspector/inspector_controller.hpp"
#include "inspector/inspector_view.hpp"
#include "inspector/list_thumbnails.hpp"
#include "inspector/preview_backend.hpp"
#include "inspector/texture_cache.hpp"
#include "inspector/thumbnail_scheduler.hpp"
#include "keymap/keymap_editor_view.hpp"
#include "menu/context_menu_view.hpp"
#include "menu/openwith_popup.hpp"
#include "ops/app_trash.hpp"
#include "ops/batch_rename_view.hpp"
#include "ops/clipboard.hpp"
#include "ops/file_ops.hpp"
#include "ops/ops_view.hpp"
#include "ops/sdl_clipboard.hpp"
#include "palette/palette_view.hpp"
#include "path/copy_path.hpp"
#include "path/path_worker.hpp"
#include "platform/outgoing_drag.hpp"
#include "render/backend_name.hpp"
#include "render/renderer.hpp"
#include "render/sdl_handles.hpp"
#include "search/content_search_overlay.hpp"
#include "search/filter_rules.hpp"
#include "search/flatten_walk.hpp"
#include "search/index_service.hpp"
#include "shell/debug_volumes.hpp"
#include "shell/painter.hpp"
#include "shell/ui_settings.hpp"
#include "sidebar/places_model.hpp"
#include "sidebar/sidebar_model.hpp"
#include "sidebar/sidebar_view.hpp"
#include "text/font_cache.hpp"
#include "text/text_renderer.hpp"
#include "theme/theme.hpp"
#include "trash/trash_model.hpp"
#include "trash/trash_view.hpp"
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
    // Phase 6: previews. The service's workers push SDL events (destroyed before SDL); textures
    // live on the renderer (destroyed before it).
    std::unique_ptr<preview::IPreviewService> previews;
    std::unique_ptr<ui::inspector::TextureCache> textures;
    ui::inspector::ThumbnailScheduler thumbs{nullptr};
    std::unique_ptr<ui::inspector::ListThumbnails> list_thumbs;
    ui::inspector::Controller inspector{nullptr};
    ui::inspector::InspectorView inspector_view;
    ui::inspector::DockLayout dock;
    ui::inspector::InspectorOutput inspector_out;
    std::vector<preview::Completion> completions;
    std::unordered_map<std::string, preview::SourceLocation> locations; // by folder
    std::size_t locations_volumes = static_cast<std::size_t>(-1);
    bool inspector_focus = false;  // keyboard focus in the Inspector (after a click)
    double inspector_max_ms = 0.0; // longest frame while the Inspector showed (dump)
    std::uint64_t locked_seen_content = 0;

    CommandQueue queue;
    CommandRegistry registry;
    Keymap keymap;

    std::unique_ptr<platform::IAppLauncher> launcher; // outlives the workspace
    std::unique_ptr<ui::PathWorker> path_worker;      // GoTo / address bar jobs
    std::unique_ptr<ui::SessionWriter> writer;        // outlives nothing it reads (copies)
    std::unique_ptr<ui::IndexService> indexes;        // Phase 5; outlives the workspace
    std::unique_ptr<ui::Workspace> workspace;
    // Phase 7: file operations (docs/architecture.md "Phase 7"). Declared after the SDL members
    // so the operation queue's workers (which push SDL events) stop before SDL is torn down.
    ui::MemoryClipboard memory_clipboard;
    ui::SdlClipboard sdl_clipboard;
    ui::IClipboard* clipboard = nullptr;
    std::function<void()> wake_cb;
    ui::TrashSource trash_source = ui::TrashSource::None;
    Path home_trash_root; // protected (ops.hpp S13); empty without a trash
    std::unique_ptr<ui::TrashModel> trash_model;
    std::unique_ptr<ui::FileOps> file_ops;
    std::set<std::uint32_t> trash_tabs; // tabs in Trash mode (runtime only)
    ui::OpsView ops_view;
    ui::TrashView trash_view;
    ui::FileDrag drag;
    ui::IncomingDrop incoming;
    ui::DropEval incoming_eval;
    fs::DirEntry incoming_first;
    std::vector<ui::DropSlot> slots; // this frame's drop slots (cleared at draw start)
    fs::DirEntry drag_first;
    bool drag_debug_hold = false; // scripted drags stay alive without a pressed button
    // Outgoing drags to other programs (X11 XDND source, Wayland attempt): created with the
    // window, started when a drag leaves it.
    std::unique_ptr<ui::OutgoingDrag> outgoing;
    bool outgoing_tried = false;              // begin() was attempted for this drag
    bool outgoing_live = false;               // the other program is the target now
    Modifiers release_mods = Modifiers::None; // held when the button went up (a key may follow)
    float window_w = 0.0F; // px, last frame (hit test of "the pointer left the window")
    float window_h = 0.0F;
    struct PendingDrop {
        std::vector<Path> items;
        Path folder;
    };
    std::optional<PendingDrop> pending_drop; // an Alt drop waiting for its menu choice
    struct PendingSelect {
        Path dir;
        std::vector<std::string> names;
        double deadline = 0.0;
    };
    std::vector<PendingSelect> pending_select;
    struct PendingRename {
        layout::TabId tab;
        Path dir;
        std::string name;
        double deadline = 0.0;
    };
    std::optional<PendingRename> pending_rename;
    std::vector<Path> protected_mounts; // last mount point set handed to the queue
    std::uint64_t rows_trash_serial = 0;
    bool rows_trash_active = false;
    ui::SidebarTrash rows_trash;
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
    // Phase 8: user keymap, sequences, command palette, searchable context menu, Open With.
    Keymap default_keymap;
    KeymapFile keymap_file;
    SequenceMatcher seq_matcher;
    palette::History palette_history;
    menu::Pins menu_pins;
    bool persist_phase8 = false;
    bool keymap_dirty = false;
    bool history_dirty = false;
    bool pins_dirty = false;
    // Phase 9A: hierarchical per-folder view settings (Global / Hierarchy / Folder).
    view::ViewSettingsStore view_settings;
    bool view_settings_dirty = false;
    std::unordered_map<std::uint32_t, std::uint64_t> tab_view_serials;
    void sync_tab_view_settings(ui::BrowserModel& m, std::uint32_t tab_val);
    void persist_view_change(ui::BrowserModel& m);
    // Phase 9B: appearance settings, custom JSON themes, and interactive batch rename.
    appearance::AppearanceSettings appearance_settings;
    std::vector<appearance::ThemeScheme> custom_themes;
    bool appearance_dirty = false;
    ui::Palette previous_palette = ui::palette(Theme::Dark);
    ui::BatchRenameOverlay batch_rename_overlay;
    [[nodiscard]] ui::Palette resolve_palette(const std::string& theme_id,
                                              Theme& out_base) const noexcept;
    void apply_appearance_settings();
    void open_batch_rename_overlay();
    ui::PaletteOverlay palette_overlay;
    ui::ContextMenuOverlay context_menu;
    ui::KeymapSettingsOverlay keymap_settings;
    ui::OpenWithOverlay openwith_overlay;
    // Phase 10: single-instance IPC, GVfs, Git status, C ABI plugins, Content Search overlay.
    std::unique_ptr<platform::ISingleInstanceServer> ipc_server;
    std::unique_ptr<platform::IGvfsBackend> gvfs_backend;
    std::unique_ptr<git::GitStatusService> git_service;
    plugins::PluginManager plugin_manager;
    Path plugin_dir;
    ui::ContentSearchOverlay content_search_overlay;
    std::vector<platform::GvfsMount> gvfs_mounts;
    Path last_git_dir;
    std::vector<Path> openwith_targets;
    std::vector<openwith::Handler> openwith_handlers;
    bool reset_keymap_confirm = false;
    int reset_confirm_focus = 0; // 0 = Cancel (safe default), 1 = Reset
    std::string seq_status_text;
    bool wayland = false;
    float content_scale = 1.0F; // X11: window size = logical size * content_scale
    bool text_input_on = false;
    ui::PointerInput pointer;
    std::vector<ui::Intent> intents;
    Path home;
    Uint32 wake_event = 0;
    bool focused = true;
    ui::Density density = ui::Density::Compact; // from ui::ui_settings() at create
    // Phase 5: the tab whose filter field has the keyboard (only while it is the active tab),
    // and the folder it showed when it got focus (navigation hands the keyboard back).
    layout::TabId filter_tab;
    Path filter_dir;
    std::vector<std::string> indexed_mounts; // last volume set handed to the index service
    bool first_frame_done = false;
    // Flatten acceptance benchmark (NAVIGATOR_DEBUG_BENCH_FLATTEN).
    bool bench_flatten = false;
    int flat_phase = 0;
    double flat_t0 = 0.0;
    bool flat_filtered = false;
    bool flat_cancelled = false;
    double flat_cancel_ms = -1.0;
    double flat_exit_ms = -1.0;
    std::uint64_t flat_found = 0;
    std::vector<double> flat_frames;

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
    void handle_text(std::string_view typed);
    [[nodiscard]] bool filter_focused() const;
    void focus_filter(bool select_all);
    void filter_escape();
    void flatten_volume() const;
    [[nodiscard]] Path volume_root(const Path& dir) const;
    [[nodiscard]] std::shared_ptr<search::IIndex> volume_index(const Path& dir, Path& root) const;
    [[nodiscard]] bool shows_index_status() const;
    void rebuild_index();
    void sync_index_volumes();
    void bench_flatten_step(double frame_ms);
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
    // Phase 6.
    void init_previews(const AppConfig& config);
    void previews_frame_start(double t_ms);
    [[nodiscard]] ui::inspector::Subject inspector_subject() const;
    [[nodiscard]] preview::SourceLocation location_for(const Path& path);
    [[nodiscard]] ui::inspector::Item item_for(const ui::BrowserModel& m, std::size_t row);
    bool route_inspector(const Command& command);
    void inspector_command(const Command& command);
    void set_inspector_visible(bool visible);
    void draw_inspector(const ui::Painter& painter, const ui::Palette& pal,
                        const ui::PointerInput& ptr, double t_ms, double dt_ms, Timestamp now);
    void apply_inspector_output();
    [[nodiscard]] bool previews_busy() const noexcept;
    // Phase 7.
    void init_file_ops(const AppConfig& config);
    void sync_protected_paths();
    [[nodiscard]] ui::FileTarget file_target() const;
    [[nodiscard]] static bool is_file_command(CommandId id) noexcept;
    void file_command(const Command& command);
    bool trash_command(const Command& command);
    [[nodiscard]] bool trash_active() const;
    void enter_trash_view();
    void leave_trash_view();
    bool sheet_key(const KeyChord& chord) const;
    void begin_inline_rename();
    [[nodiscard]] std::function<bool(std::string_view)> name_taken() const;
    void apply_reconcile(double t_ms);
    void process_pending(double t_ms);
    void drag_begin(std::uint32_t tab, std::size_t row);
    void drag_frame_end(double t_ms);
    void apply_drop(std::vector<Path> items, const ui::DropEval& eval, float x, float y);
    void incoming_update();
    void handle_drop_event(const SDL_Event& event);
    void quit_request();
    [[nodiscard]] std::vector<ui::DropSlot>::const_iterator find_slot(std::string_view spec) const;
    // Phase 8 helpers.
    [[nodiscard]] bool phase8_overlay_open() const noexcept {
        return palette_overlay.is_open() || context_menu.is_open() || keymap_settings.is_open() ||
               openwith_overlay.is_open() || reset_keymap_confirm;
    }
    void rebuild_keymap_from_file();
    [[nodiscard]] CommandContext capture_context() const;
    void open_palette_overlay();
    void open_context_menu_at(menu::MenuKind kind, float x, float y, const menu::Target& target);
    void open_openwith_overlay();
    void launch_openwith_handler(const openwith::Handler& h, const std::vector<Path>& paths,
                                 std::string_view mime, bool set_default);
    void apply_palette_action(const ui::PaletteAction& act);
    void apply_menu_action(const ui::MenuAction& act);
    void flush_phase8_files();
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
    if (auto r = load_default_keymap(default_keymap); !r) {
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
    persist_phase8 = persist;
    // Phase 8: load user keymap, palette history, and menu pins from config_root (if allowed).
    if (restore) {
        if (auto kf = load_keymap_file(keymap_file_path(config_root), nullptr); kf) {
            keymap_file = std::move(*kf);
        }
        if (auto ph = palette::load_history(palette::history_file(config_root)); ph) {
            palette_history = std::move(*ph);
        }
        if (auto mp = menu::load_pins(menu::pins_file(config_root)); mp) {
            menu_pins = std::move(*mp);
        }
        if (auto vs = view::load_view_settings(view::view_settings_file(config_root)); vs) {
            view_settings = std::move(*vs);
        }
        custom_themes = appearance::list_custom_themes(appearance::themes_dir(config_root));
        if (auto ap = appearance::load_appearance_settings(appearance::settings_file(config_root));
            ap) {
            appearance_settings = std::move(*ap);
            if (!config.animations) {
                appearance_settings.animations = false;
            }
            apply_appearance_settings();
        } else {
            appearance_settings.theme_id = (config.theme == Theme::Light)
                                               ? std::string(appearance::kBuiltinLightId)
                                               : std::string(appearance::kBuiltinDarkId);
            appearance_settings.animations = animations;
            previous_palette = resolve_palette(appearance_settings.theme_id, theme);
        }
    } else {
        appearance_settings.theme_id = (config.theme == Theme::Light)
                                           ? std::string(appearance::kBuiltinLightId)
                                           : std::string(appearance::kBuiltinDarkId);
        appearance_settings.animations = animations;
        previous_palette = resolve_palette(appearance_settings.theme_id, theme);
    }
    rebuild_keymap_from_file();

    // Phase 10: Single-instance IPC server, Git status service, GVfs backend, C ABI plugins.
    if (config.enable_ipc && (config.max_frames == 0 || !config.ipc_socket_path.empty())) {
        std::optional<Path> sock;
        if (!config.ipc_socket_path.empty()) {
            sock = Path(config.ipc_socket_path);
        } else if (auto def = platform::default_ipc_socket_path(); def) {
            sock = std::move(*def);
        }
        if (sock) {
            if (auto srv = platform::create_single_instance_server(*sock); srv) {
                ipc_server = std::move(*srv);
            }
        }
    }
    if (config.enable_git) {
        git_service = std::make_unique<git::GitStatusService>();
    }
    if (config.enable_gvfs) {
        if (auto gvfs = platform::create_gvfs_backend(); gvfs) {
            gvfs_backend = std::move(*gvfs);
            if (auto mounts = gvfs_backend->list_mounts(); mounts) {
                gvfs_mounts = std::move(*mounts);
            }
        }
    }
    plugin_dir = !config.plugin_dir.empty() ? Path(config.plugin_dir) : (config_root / "plugins");
    if (config.enable_plugins && !plugin_dir.empty()) {
        std::vector<std::string> plugin_errs;
        (void)plugin_manager.load_directory(plugin_dir, &plugin_errs);
        for (const std::string& e : plugin_errs) {
            SDL_Log("Navigator: plugin load skipped: %s", e.c_str());
        }
    }

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

    outgoing = ui::create_outgoing_drag(window.get());
    SDL_Log("Navigator: outgoing drag: %s", outgoing
                                                ? (outgoing->backend() == ui::OutgoingBackend::X11
                                                       ? "X11 (XDND)"
                                                       : "Wayland (data device attempt)")
                                                : "not available (clipboard path)");
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
    if (const char* sw = env("NAVIGATOR_DEBUG_SLOW_WALK")) {
        // Development: small, slow batches so a running flatten can be looked at.
        const auto delay = std::chrono::milliseconds(std::max(1L, std::strtol(sw, nullptr, 10)));
        wo.model.walk_factory =
            [delay](const fs::WalkOptions& o) -> Result<std::unique_ptr<ui::WalkSource>> {
            fs::WalkOptions slow = o;
            slow.batch_size = 48;
            Result<std::unique_ptr<ui::WalkSource>> inner = ui::default_walk_factory()(slow);
            if (!inner) {
                return inner;
            }
            struct Slow final : ui::WalkSource {
                std::unique_ptr<ui::WalkSource> in;
                std::chrono::milliseconds wait;
                Result<bool> next_batch(fs::WalkBatch& out, std::stop_token stop) override {
                    std::this_thread::sleep_for(wait);
                    return in->next_batch(out, std::move(stop));
                }
                [[nodiscard]] const fs::WalkStats& stats() const noexcept override {
                    return in->stats();
                }
            };
            auto w = std::make_unique<Slow>();
            w->in = std::move(*inner);
            w->wait = delay;
            return std::unique_ptr<ui::WalkSource>(std::move(w));
        };
    }
    wo.home = home;
    wo.row_dp = ui::density_metrics(density).row_height;
    wo.config_root = config_root;
    // Phase 5: the name-index service. Headless runs without --cache-dir never index (and never
    // touch ~/.cache), like the session rule above.
    {
        ui::IndexService::Options io;
        io.enabled = config.enable_index && (config.max_frames <= 0 || !config.cache_dir.empty());
        if (!config.cache_dir.empty()) {
            io.cache_root = Path(std::string(config.cache_dir));
        } else if (auto c = platform::user_dir(platform::UserDir::Cache); c) {
            io.cache_root = *c / "navigator";
        }
        if (io.cache_root.empty()) {
            io.enabled = false;
        }
        io.watcher_factory = [] { return create_inotify_watcher(); };
        io.wake = wake_fn;
        indexes = std::make_unique<ui::IndexService>(std::move(io));
        if (const char* root = env("NAVIGATOR_DEBUG_INDEX_ROOT"); root != nullptr) {
            VolumeIdentity id;
            id.uuid = "debug-index";
            id.root = "/";
            id.mount_point = Path(root);
            (void)indexes->add_root(Path(root), id);
        }
    }
    bench_flatten = env("NAVIGATOR_DEBUG_BENCH_FLATTEN") != nullptr;
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
    wake_cb = wake_fn;
    init_previews(config);
    init_file_ops(config);
    return ok();
}

// ---------------------------------------------------------------------------------------------
// Phase 7: file operations
// ---------------------------------------------------------------------------------------------

void App::Impl::init_file_ops(const AppConfig& config) {
    const bool headless = config.max_frames > 0;
    // A headless run never touches the desktop clipboard or the owner's trash (AppConfig).
    clipboard = headless ? static_cast<ui::IClipboard*>(&memory_clipboard)
                         : static_cast<ui::IClipboard*>(&sdl_clipboard);
    ui::AppTrash at = ui::create_app_trash(config.trash_dir, headless);
    if (!at.note.empty()) {
        SDL_Log("Navigator: %s", at.note.c_str());
    }
    trash_source = at.source;
    ui::FileOpsOptions fo;
    fo.trash = at.trash;
    fo.clipboard = clipboard;
    fo.wake = wake_cb;
    fo.new_folder_name = [] { return std::string(tr("file.new_folder_default")); };
    fo.is_mounted = [this](const Path& mount) {
        const std::vector<VolumeInfo>& volumes = places->volumes();
        if (volumes.empty() || mount == Path("/")) {
            return true; // the volume list is not known yet: do not block an undo on a guess
        }
        return std::ranges::any_of(volumes,
                                   [&](const VolumeInfo& v) { return v.mount_point == mount; });
    };
    if (const char* d = env("NAVIGATOR_DEBUG_OPS_DELAY_MS")) {
        const auto delay = std::chrono::milliseconds(std::max(0L, std::strtol(d, nullptr, 10)));
        fo.fault_injector = [delay](const Path&) {
            std::this_thread::sleep_for(delay);
            return true;
        };
        fo.progress_interval = std::chrono::milliseconds(50);
    }
    // Protected paths (S13): the home directory and its ancestors, the mount points, and the
    // trash directories (lexical; refreshed when the volume list changes).
    fo.protected_paths.home = home;
    if (at.source == ui::TrashSource::Injected) {
        home_trash_root = Path(std::string(config.trash_dir));
    } else if (at.source == ui::TrashSource::System) {
        if (auto dd = platform::data_dirs(); dd && !dd->empty()) {
            home_trash_root = dd->front() / "Trash";
        }
    }
    if (!home_trash_root.empty()) {
        fo.protected_paths.trash_roots.push_back(home_trash_root);
    }
    file_ops = std::make_unique<ui::FileOps>(std::move(fo));
    ui::TrashModel::Options to;
    to.trash = at.trash;
    to.wake = wake_cb;
    trash_model = std::make_unique<ui::TrashModel>(std::move(to));
    if (at.trash) {
        trash_model->refresh(); // the count of the sidebar row
    }
}

void App::Impl::sync_protected_paths() {
    if (!file_ops || !places) {
        return;
    }
    std::vector<Path> mounts;
    for (const VolumeInfo& v : places->volumes()) {
        if (!v.mount_point.empty()) {
            mounts.push_back(v.mount_point);
        }
    }
    if (mounts == protected_mounts) {
        return;
    }
    protected_mounts = mounts;
    ops::ProtectedPaths pp;
    pp.home = home;
    pp.mount_points = std::move(mounts);
    // The home trash root is the one the queue was created with; per-volume trashes sit below
    // their mount points (.Trash/<uid> and .Trash-<uid>, trash.hpp T4).
    const auto uid = static_cast<std::uint32_t>(::getuid());
    for (const Path& m : pp.mount_points) {
        pp.trash_roots.push_back(trash::user_topdir_trash(m, uid));
        pp.trash_roots.push_back(trash::shared_topdir_trash(m, uid));
    }
    if (!home_trash_root.empty()) {
        pp.trash_roots.push_back(home_trash_root);
    }
    file_ops->set_protected_paths(std::move(pp));
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

void App::Impl::flush_phase8_files() {
    if (!persist_phase8 || config_root.empty()) {
        return;
    }
    if (keymap_dirty) {
        keymap_dirty = false;
        (void)save_keymap_file(keymap_file_path(config_root), keymap_file);
    }
    if (history_dirty) {
        history_dirty = false;
        (void)palette::save_history(palette::history_file(config_root), palette_history);
    }
    if (pins_dirty) {
        pins_dirty = false;
        (void)menu::save_pins(menu::pins_file(config_root), menu_pins);
    }
    if (view_settings_dirty) {
        view_settings_dirty = false;
        (void)view::save_view_settings(view::view_settings_file(config_root), view_settings);
    }
    if (appearance_dirty) {
        appearance_dirty = false;
        (void)appearance::save_appearance_settings(appearance::settings_file(config_root),
                                                   appearance_settings);
    }
}

ui::Palette App::Impl::resolve_palette(const std::string& theme_id,
                                       Theme& out_base) const noexcept {
    if (theme_id == appearance::kBuiltinLightId) {
        out_base = Theme::Light;
        return ui::palette(Theme::Light);
    }
    if (theme_id == appearance::kBuiltinDarkId) {
        out_base = Theme::Dark;
        return ui::palette(Theme::Dark);
    }
    for (const appearance::ThemeScheme& sc : custom_themes) {
        if (sc.id == theme_id) {
            out_base = sc.base;
            return ui::palette_from_scheme(sc);
        }
    }
    out_base = Theme::Dark;
    return ui::palette(Theme::Dark);
}

void App::Impl::apply_appearance_settings() {
    Theme prev_base = theme;
    previous_palette = resolve_palette(appearance_settings.theme_id, prev_base);
    Theme next_base = theme;
    (void)resolve_palette(appearance_settings.theme_id, next_base);
    previous_theme = theme;
    theme = next_base;
    density = ui::to_ui_density(appearance_settings.density);
    animations = appearance_settings.animations;
    if (!animations) {
        fade_start_ms = -1.0;
    }
    dirty = true;
}

void App::Impl::open_batch_rename_overlay() {
    if (trash_active() || file_ops->sheet_open()) {
        return;
    }
    ui::BrowserModel& m = workspace->active_model();
    if (!m.loaded() || m.current_dir().empty()) {
        return;
    }
    const std::size_t n = m.row_count();
    if (n == 0) {
        return;
    }
    std::vector<ops::BatchRenameItem> items;
    std::vector<std::string> siblings;
    siblings.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const fs::DirEntry& e = m.row_entry(i);
        siblings.push_back(e.name);
        if (m.is_selected(i)) {
            ops::BatchRenameItem it;
            it.path = m.row_path(i);
            it.original_name = e.name;
            it.is_directory = fs::is_directory_like(e);
            it.mtime = e.mtime;
            it.size = e.size;
            items.push_back(std::move(it));
        }
    }
    if (items.empty() && m.cursor() < n) {
        const std::size_t c = m.cursor();
        const fs::DirEntry& e = m.row_entry(c);
        ops::BatchRenameItem it;
        it.path = m.row_path(c);
        it.original_name = e.name;
        it.is_directory = fs::is_directory_like(e);
        it.mtime = e.mtime;
        it.size = e.size;
        items.push_back(std::move(it));
    }
    if (items.empty()) {
        return;
    }
    seq_matcher.reset();
    seq_status_text.clear();
    palette_overlay.close();
    context_menu.close();
    openwith_overlay.close();
    keymap_settings.close();
    batch_rename_overlay.open(std::move(items), std::move(siblings));
    dirty = true;
}

void App::Impl::sync_tab_view_settings(ui::BrowserModel& m, std::uint32_t tab_val) {
    if (!m.loaded() || m.current_dir().empty()) {
        return;
    }
    const std::uint64_t loc_s = m.location_serial();
    auto it = tab_view_serials.find(tab_val);
    if (it != tab_view_serials.end() && it->second == loc_s) {
        return;
    }
    tab_view_serials[tab_val] = loc_s;
    const view::ResolvedViewSettings resolved = view_settings.resolve(m.current_dir());
    m.apply_view_settings(resolved.settings, resolved.scope, resolved.rule_path);
}

void App::Impl::persist_view_change(ui::BrowserModel& m) {
    if (!m.current_dir().empty()) {
        view_settings.apply_change(m.current_dir(), m.view_settings());
        view_settings_dirty = true;
    }
}

void App::Impl::persist_tick(double t_ms) {
    if (places) {
        places->tick(t_ms);
    }
    if (keymap_dirty || history_dirty || pins_dirty || view_settings_dirty || appearance_dirty) {
        flush_phase8_files();
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
    flush_phase8_files();
    // Phase 7: running operations stop at their next check (S9); finished items stay done.
    if (file_ops) {
        file_ops->shutdown();
    }
    if (places) {
        places->final_save();
    }
    // Phase 5: indexes save and stop off the UI thread within a bounded wait; walks still
    // blocked in a syscall are left detached (walk.hpp).
    if (workspace) {
        workspace->cancel_walks();
    }
    if (indexes) {
        indexes->shutdown(std::chrono::milliseconds(1500));
    }
    (void)ui::FlattenWalk::wait_for_exit(std::chrono::milliseconds(300));
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
    // Phase 5: a running search streams results and animates its progress.
    return max_frames > 0 || show_fps || fade_start_ms >= 0.0 || view.animating() || bench ||
           sidebar.animating() || (workspace && workspace->searching()) || bench_flatten ||
           previews_busy() ||
           (file_ops &&
            (file_ops->busy() || file_ops->sheet_open() || file_ops->notice().has_value() ||
             ops_view.animating() || file_ops->confirm_stats().has_value())) ||
           drag.dragging() || incoming.active() || pending_rename.has_value() ||
           !pending_select.empty() || (trash_model && trash_model->loading());
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
            if (content_search_overlay.open()) {
                content_search_overlay.on_text(event.text.text);
                dirty = true;
            } else {
                handle_text(event.text.text);
            }
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        pointer.x = event.motion.x * pixel_density;
        pointer.y = event.motion.y * pixel_density;
        pointer.inside = true;
        if (drag.dragging()) {
            pointer.mods =
                ui::translate_modifiers(SDL_GetModState()); // Ctrl / Shift / Alt mid-drag
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        seq_matcher.reset();
        seq_status_text.clear();
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
            release_mods = pointer.mods; // the modifiers of the drop, even if a key goes up next
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
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_POSITION:
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_DROP_COMPLETE: handle_drop_event(event); break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: pointer.inside = false; break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER: pointer.inside = true; break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: focused = true; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        focused = false;
        view.cancel_drag();
        drag.cancel();
        drag_debug_hold = false;
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

bool App::Impl::sheet_key(const KeyChord& chord) const {
    if (!file_ops->sheet_open()) {
        return false;
    }
    ui::ConflictSheet* cs = file_ops->conflict_sheet();
    const bool editing =
        file_ops->sheet_kind() == ui::SheetKind::Conflict && cs != nullptr && cs->editing_name;
    const bool plain = chord.mods == Modifiers::None;
    if (chord.key == Key::Enter || chord.key == Key::KeypadEnter) {
        if (plain || editing) {
            file_ops->sheet_activate();
            return true;
        }
        return false;
    }
    if (chord.key == Key::Escape && plain) {
        file_ops->sheet_escape();
        return true;
    }
    if (editing) {
        const ui::FieldKeyResult r = ui::apply_field_key(cs->name, chord, [] {
            std::string out;
            if (char* t = SDL_GetClipboardText(); t != nullptr) {
                out = t;
                SDL_free(t);
            }
            return out;
        });
        if (r.clipboard) {
            set_clipboard(*r.clipboard);
        }
        if (r.changed) {
            file_ops->conflict_name_edited();
        }
        return r.handled;
    }
    if (chord.key == Key::Tab && (plain || chord.mods == Modifiers::Shift)) {
        file_ops->sheet_focus(plain ? 1 : -1);
        return true;
    }
    if ((chord.key == Key::Right || chord.key == Key::Left) && plain) {
        file_ops->sheet_focus(chord.key == Key::Right ? 1 : -1);
        return true;
    }
    if (chord.key == Key::Space && plain) {
        file_ops->sheet_toggle_apply_all();
        return true;
    }
    if (chord.key == Key::F2 && plain) {
        file_ops->conflict_begin_name_edit();
        return true;
    }
    return false;
}

void App::Impl::handle_key(const KeyChord& chord, bool repeat) {
    // Phase 8: Reset Keymap confirmation sheet (initial focus on Cancel).
    if (reset_keymap_confirm) {
        dirty = true;
        if (chord.key == Key::Escape && chord.mods == Modifiers::None) {
            reset_keymap_confirm = false;
            return;
        }
        if ((chord.key == Key::Tab || chord.key == Key::Left || chord.key == Key::Right) &&
            (chord.mods == Modifiers::None || chord.mods == Modifiers::Shift)) {
            reset_confirm_focus = 1 - reset_confirm_focus;
            return;
        }
        if ((chord.key == Key::Enter || chord.key == Key::KeypadEnter) &&
            chord.mods == Modifiers::None) {
            if (reset_confirm_focus == 1 && reset_all(keymap_file)) {
                keymap_dirty = true;
                rebuild_keymap_from_file();
            }
            reset_keymap_confirm = false;
            return;
        }
        return;
    }
    // Phase 9B: Batch Rename overlay.
    if (batch_rename_overlay.is_open()) {
        dirty = true;
        if (ui::TextEdit* f = batch_rename_overlay.active_field(); f != nullptr) {
            const ui::FieldKeyResult r = ui::apply_field_key(*f, chord, [] {
                std::string out;
                if (char* t = SDL_GetClipboardText(); t != nullptr) {
                    out = t;
                    SDL_free(t);
                }
                return out;
            });
            if (r.handled) {
                if (r.clipboard) {
                    set_clipboard(*r.clipboard);
                }
                if (r.changed) {
                    batch_rename_overlay.field_changed();
                }
                return;
            }
        }
        if (ui::field_consumes_as_text(chord)) {
            return;
        }
        if (!repeat) {
            const ui::BatchRenameEvent ev = batch_rename_overlay.handle_key(chord);
            if (ev.request_paste_clipboard) {
                if (char* t = SDL_GetClipboardText(); t != nullptr) {
                    batch_rename_overlay.paste_text(t);
                    SDL_free(t);
                }
            }
            if (ev.apply_request) {
                file_ops->submit_batch_rename(*ev.apply_request);
            }
        }
        return;
    }
    // Phase 8: Keymap Settings overlay (including shortcut capture mode).
    if (keymap_settings.is_open()) {
        dirty = true;
        if (!keymap_settings.is_capturing()) {
            if (const auto cmd = resolve_command(keymap, registry, chord);
                cmd && (cmd->id == CommandId::OpenSettings || cmd->id == CommandId::EditKeymap)) {
                keymap_settings.close();
                return;
            }
            if (ui::TextEdit* f = keymap_settings.active_field(); f != nullptr) {
                const ui::FieldKeyResult r = ui::apply_field_key(*f, chord, [] {
                    std::string out;
                    if (char* t = SDL_GetClipboardText(); t != nullptr) {
                        out = t;
                        SDL_free(t);
                    }
                    return out;
                });
                if (r.handled) {
                    if (r.clipboard) {
                        set_clipboard(*r.clipboard);
                    }
                    if (r.changed) {
                        keymap_settings.search_changed();
                    }
                    return;
                }
            }
            if (ui::field_consumes_as_text(chord)) {
                return;
            }
        }
        if (!repeat) {
            const ui::KeymapEditorEvent ev =
                keymap_settings.handle_key(chord, keymap_file, keymap, default_keymap, registry);
            if (ev.keymap_dirty) {
                keymap_dirty = true;
                rebuild_keymap_from_file();
            }
            if (ev.enqueue_cmd) {
                queue.enqueue(*ev.enqueue_cmd);
            }
        }
        return;
    }
    // Phase 10: Content Search overlay captures keyboard while open.
    if (content_search_overlay.open()) {
        dirty = true;
        if (const auto picked = content_search_overlay.on_key(chord.key, chord.mods); picked) {
            leave_trash_view();
            workspace->navigate_active(picked->parent_path(), picked->filename().native());
        }
        return;
    }
    // Phase 8: Command Palette overlay.
    if (palette_overlay.is_open()) {
        dirty = true;
        if (const auto cmd = resolve_command(keymap, registry, chord);
            cmd && cmd->id == CommandId::OpenPalette && !repeat) {
            palette_overlay.close();
            return;
        }
        if (ui::TextEdit* f = palette_overlay.active_field(); f != nullptr) {
            const ui::FieldKeyResult r = ui::apply_field_key(*f, chord, [] {
                std::string out;
                if (char* t = SDL_GetClipboardText(); t != nullptr) {
                    out = t;
                    SDL_free(t);
                }
                return out;
            });
            if (r.handled) {
                if (r.clipboard) {
                    set_clipboard(*r.clipboard);
                }
                if (r.changed) {
                    palette_overlay.query_changed(palette_history);
                }
                return;
            }
        }
        if (ui::field_consumes_as_text(chord)) {
            return;
        }
        if (const auto act = palette_overlay.handle_key(chord, palette_history)) {
            apply_palette_action(*act);
        }
        return;
    }
    // Phase 8: Searchable Context Menu overlay.
    if (context_menu.is_open()) {
        dirty = true;
        if (ui::TextEdit* f = context_menu.active_field(); f != nullptr) {
            const ui::FieldKeyResult r = ui::apply_field_key(*f, chord, [] {
                std::string out;
                if (char* t = SDL_GetClipboardText(); t != nullptr) {
                    out = t;
                    SDL_free(t);
                }
                return out;
            });
            if (r.handled) {
                if (r.clipboard) {
                    set_clipboard(*r.clipboard);
                }
                if (r.changed) {
                    context_menu.query_changed();
                }
                return;
            }
        }
        if (ui::field_consumes_as_text(chord)) {
            return;
        }
        if (const auto act = context_menu.handle_key(chord)) {
            apply_menu_action(*act);
        }
        return;
    }
    // Phase 8: Open With popup overlay.
    if (openwith_overlay.is_open()) {
        dirty = true;
        if (ui::TextEdit* f = openwith_overlay.active_field(); f != nullptr) {
            const ui::FieldKeyResult r = ui::apply_field_key(*f, chord, [] {
                std::string out;
                if (char* t = SDL_GetClipboardText(); t != nullptr) {
                    out = t;
                    SDL_free(t);
                }
                return out;
            });
            if (r.handled) {
                if (r.clipboard) {
                    set_clipboard(*r.clipboard);
                }
                if (r.changed) {
                    openwith_overlay.query_changed();
                }
                return;
            }
        }
        if (ui::field_consumes_as_text(chord)) {
            return;
        }
        if (const auto act = openwith_overlay.handle_key(chord)) {
            launch_openwith_handler(act->handler, act->paths, act->primary_mime,
                                    act->set_as_default);
        }
        return;
    }
    // Phase 7: a conflict / confirmation / quit sheet takes Enter, Escape, Tab, Space first.
    if (sheet_key(chord)) {
        dirty = true;
        return;
    }
    // Delete in the layout picker deletes the highlighted layout (popup-local, docs/keymap.md
    // "Popups"), before its filter field sees the key.
    if (chord == KeyChord{Key::Delete, Modifiers::None} && !repeat && view.delete_key(intents)) {
        return;
    }
    // Phase 6: keys local to a focused Inspector (fit, 1:1, zoom, grid, minimap).
    if (inspector_focus && inspector.visible() && !view.popup_open() && text_field() == nullptr) {
        if (const std::optional<ui::inspector::LocalKey> k =
                ui::inspector::inspector_local_key(chord);
            k) {
            dirty = true;
            switch (*k) {
            case ui::inspector::LocalKey::Fit: queue.enqueue(CommandId::InspectorZoomFit); break;
            case ui::inspector::LocalKey::OneToOne:
                queue.enqueue(CommandId::InspectorZoom1to1);
                break;
            case ui::inspector::LocalKey::ZoomIn: inspector_view.zoom_step(1); break;
            case ui::inspector::LocalKey::ZoomOut: inspector_view.zoom_step(-1); break;
            case ui::inspector::LocalKey::Grid:
                queue.enqueue(CommandId::ToggleInspectorGrid);
                break;
            case ui::inspector::LocalKey::Minimap:
                queue.enqueue(CommandId::ToggleInspectorMinimap);
                break;
            }
            return;
        }
    }
    // Phase 6: a focused text field takes Space / Shift+Space as text (the character comes as
    // SDL text input), so typing a space never toggles the Inspector.
    if (ui::field_consumes_as_text(chord) &&
        (filter_focused() || text_field() != nullptr || view.wants_text_input())) {
        return;
    }
    // Phase 5: the filter field takes only its editing keys (docs/keymap.md "Focus scoping
    // (Phase 5)"); arrows, Enter, Ctrl+A, Ctrl+E ... go on to the keymap and the pane.
    if (filter_focused()) {
        ui::BrowserModel& m = workspace->active_model();
        ui::TextEdit& edit = m.filter_edit();
        switch (ui::route_filter_key(chord, edit.has_selection())) {
        case ui::FilterKey::Edit: {
            const ui::FieldKeyResult r = ui::apply_field_key(edit, chord, [] {
                std::string out;
                if (char* t = SDL_GetClipboardText(); t != nullptr) {
                    out = t;
                    SDL_free(t);
                }
                return out;
            });
            dirty = true;
            if (r.clipboard) {
                set_clipboard(*r.clipboard);
            }
            if (r.changed) {
                m.filter_edited();
            }
            return;
        }
        case ui::FilterKey::Escape:
            if (!repeat) {
                filter_escape();
            }
            dirty = true;
            return;
        case ui::FilterKey::ToList:
            filter_tab = layout::TabId{};
            dirty = true;
            return;
        case ui::FilterKey::Keymap: break;
        }
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
    // Phase 8: multi-chord key sequences (SequenceMatcher) run after focus scoping and before
    // single-chord resolution, once per key press (no auto-repeat).
    if (!repeat) {
        const SequenceResult seq_res =
            seq_matcher.feed(keymap, chord, SequenceMatcher::Clock::now());
        if (seq_res.expired) {
            seq_status_text.clear();
        }
        if (seq_res.consumed) {
            dirty = true;
            if (seq_res.kind == SequenceResult::Kind::Pending) {
                seq_status_text = ui::shortcut_text(KeySequence{seq_res.sequence}) + " …";
            } else if (seq_res.kind == SequenceResult::Kind::Match && seq_res.binding) {
                seq_status_text.clear();
                if (const auto cmd = to_command(registry, *seq_res.binding)) {
                    queue.enqueue(*cmd);
                }
            } else if (seq_res.kind == SequenceResult::Kind::Cancelled) {
                seq_status_text.clear();
            } else if (seq_res.kind == SequenceResult::Kind::Aborted) {
                seq_status_text.clear();
                std::string msg(tr("keymap.sequence_aborted"));
                if (const std::size_t pos = msg.find("{seq}"); pos != std::string::npos) {
                    msg.replace(pos, 5, ui::shortcut_text(KeySequence{seq_res.sequence}));
                }
                toast(std::move(msg));
            }
            return;
        }
    }
    if (const auto cmd = resolve_command(keymap, registry, chord); cmd) {
        // Auto-repeat moves the cursor; it never re-fires toggles or navigation.
        if (!repeat || is_cursor_command(cmd->id)) {
            queue.enqueue(*cmd);
        }
    }
}

ui::TextEdit* App::Impl::text_field() noexcept {
    if (batch_rename_overlay.is_open()) {
        return batch_rename_overlay.active_field();
    }
    if (palette_overlay.is_open()) {
        return palette_overlay.active_field();
    }
    if (context_menu.is_open()) {
        return context_menu.active_field();
    }
    if (openwith_overlay.is_open()) {
        return openwith_overlay.active_field();
    }
    if (keymap_settings.is_open()) {
        return keymap_settings.active_field();
    }
    if (file_ops && file_ops->sheet_kind() == ui::SheetKind::Conflict &&
        file_ops->conflict_sheet() != nullptr && file_ops->conflict_sheet()->editing_name) {
        return &file_ops->conflict_sheet()->name; // the typed Keep both name
    }
    if (file_ops && file_ops->inline_edit() != nullptr && !view.popup_open()) {
        return &file_ops->inline_edit()->edit; // F2 / Ctrl+Shift+N editor
    }
    if (sidebar.renaming() && !view.popup_open()) {
        return sidebar.rename_edit();
    }
    return view.active_field();
}

bool App::Impl::filter_focused() const {
    return filter_tab.valid() && filter_tab == workspace->active_tab() && !view.popup_open() &&
           !view.address_open() && !sidebar.renaming() && !sidebar_focus &&
           workspace->model(filter_tab) != nullptr;
}

void App::Impl::focus_filter(bool select_all) {
    ui::BrowserModel& m = workspace->active_model();
    if (!m.loaded()) {
        return;
    }
    focus_sidebar(false);
    view.close_address_bar();
    filter_tab = workspace->active_tab();
    filter_dir = m.current_dir();
    if (select_all) {
        m.filter_edit().select_all();
    }
}

void App::Impl::filter_escape() {
    ui::BrowserModel& m = workspace->active_model();
    const ui::FlattenProgress prog = m.flatten_progress();
    const bool searching = m.flattened() && (prog.walking || (prog.from_index && prog.organizing));
    switch (ui::filter_field_escape(searching, m.filter_active())) {
    case ui::FieldEscape::CancelSearch: m.cancel_search(); return;
    case ui::FieldEscape::ClearText: m.clear_filter(); return;
    case ui::FieldEscape::ReturnFocus: filter_tab = layout::TabId{}; return;
    }
}

void App::Impl::handle_text(std::string_view typed) {
    if (batch_rename_overlay.is_open()) {
        if (ui::TextEdit* f = batch_rename_overlay.active_field();
            f != nullptr && f->insert(typed)) {
            batch_rename_overlay.field_changed();
            dirty = true;
        }
        return;
    }
    if (ui::TextEdit* f = text_field();
        f != nullptr &&
        (file_ops->inline_edit() != nullptr || file_ops->sheet_kind() == ui::SheetKind::Conflict)) {
        if (f->insert(typed)) {
            field_edited();
        }
        return;
    }
    if (file_ops->sheet_open()) {
        return; // a sheet owns the keyboard: typing starts nothing behind it
    }
    if (sidebar.renaming() && !view.popup_open()) {
        (void)sidebar.rename_edit()->insert(typed);
        return;
    }
    if (view.wants_text_input()) {
        view.text_input(typed);
        return;
    }
    if (filter_focused()) {
        ui::BrowserModel& m = workspace->active_model();
        if (m.filter_edit().insert(typed)) {
            m.filter_edited();
        }
        return;
    }
    // Type to filter (docs/keymap.md): the list has the keyboard, no Ctrl/Alt/Super held, not a
    // leading Space. The typed text replaces the previous filter text.
    const ui::KeyboardOwner owner = ui::keyboard_owner(
        view.popup_open(), view.address_open() || sidebar.renaming(), sidebar_focus);
    const Modifiers held = ui::translate_modifiers(SDL_GetModState());
    // The Inspector's local keys (G, M, 0, 1, ...) never start type-to-filter.
    if (inspector_focus && inspector.visible()) {
        return;
    }
    if (trash_active()) {
        return; // no filter in the Trash view
    }
    if (ui::starts_type_to_filter(owner, held, typed)) {
        ui::BrowserModel& m = workspace->active_model();
        if (!m.loaded()) {
            return;
        }
        focus_filter(false);
        m.filter_edit().set({});
        if (m.filter_edit().insert(typed)) {
            m.filter_edited();
        }
    }
}

Path App::Impl::volume_root(const Path& dir) const {
    const std::vector<VolumeInfo>& volumes = places->volumes();
    if (const std::optional<std::size_t> at =
            places::containing_volume(dir, Span<const VolumeInfo>(volumes));
        at.has_value()) {
        return volumes[*at].mount_point;
    }
    return dir.root_path().empty() ? Path("/") : dir.root_path();
}

std::shared_ptr<search::IIndex> App::Impl::volume_index(const Path& dir, Path& root) const {
    // The index of the volume that HOLDS `dir`. The index of "/" does not cover a disk mounted
    // below it (it never crosses filesystems), so a shallower root does not count.
    root = volume_root(dir);
    std::shared_ptr<search::IIndex> index = indexes->index_for(dir);
    if (!index || index->root().native().size() < root.native().size()) {
        return nullptr;
    }
    root = index->root();
    return index;
}

bool App::Impl::shows_index_status() const {
    for (const layout::TabId tab : workspace->visible_tabs()) {
        const ui::BrowserModel* m = workspace->model(tab);
        if (m != nullptr && m->flatten_is_volume()) {
            return true;
        }
    }
    return false;
}

void App::Impl::flatten_volume() const {
    ui::BrowserModel& m = workspace->active_model();
    if (!m.loaded() || m.current_dir().empty()) {
        return;
    }
    Path root;
    std::shared_ptr<search::IIndex> index = volume_index(m.current_dir(), root);
    if (m.flattened() && m.flatten_root() == root && m.flatten_progress().volume) {
        m.leave_flatten();
        return;
    }
    if (index) {
        const search::IndexState state = index->status().state;
        if (state != search::IndexState::Ready && state != search::IndexState::Verifying) {
            index.reset(); // not ready: walk the volume (progress shown); the index keeps going
        }
    }
    m.flatten(ui::FlattenRequest{root, true, std::move(index)});
}

void App::Impl::rebuild_index() {
    const ui::BrowserModel& m = workspace->active_model();
    if (!indexes->enabled()) {
        toast(std::string(tr("index.disabled")));
        return;
    }
    Path root;
    const bool indexed = m.loaded() && volume_index(m.current_dir(), root) != nullptr;
    if (indexed && indexes->rebuild(m.current_dir())) {
        toast(std::string(tr("index.rebuilding")));
    } else {
        toast(std::string(tr("index.not_indexed")));
    }
}

void App::Impl::sync_index_volumes() {
    if (!indexes || !indexes->enabled() || env("NAVIGATOR_DEBUG_INDEX_ROOT") != nullptr ||
        !places->volumes_ready()) {
        return;
    }
    std::vector<std::string> mounts;
    for (const VolumeInfo& v : places->volumes()) {
        mounts.push_back(v.mount_point.string() + "\n" + v.uuid.value_or(std::string{}));
    }
    if (mounts != indexed_mounts) {
        indexed_mounts = std::move(mounts);
        (void)indexes->set_volumes(places->volumes());
    }
}

void App::Impl::field_edited() {
    if (palette_overlay.is_open()) {
        palette_overlay.query_changed(palette_history);
        return;
    }
    if (context_menu.is_open()) {
        context_menu.query_changed();
        return;
    }
    if (openwith_overlay.is_open()) {
        openwith_overlay.query_changed();
        return;
    }
    if (keymap_settings.is_open()) {
        keymap_settings.search_changed();
        return;
    }
    if (file_ops->sheet_kind() == ui::SheetKind::Conflict) {
        file_ops->conflict_name_edited();
        return;
    }
    if (file_ops->inline_edit() != nullptr && !view.popup_open()) {
        file_ops->inline_edited(name_taken());
        return;
    }
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
    if (r.kind == ui::SidebarRowKind::Place && r.place == places::PlaceKind::Trash) {
        queue.enqueue(CommandId::OpenTrash); // the Trash row opens the Trash view
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
        leave_trash_view();
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
    if (r.kind == ui::SidebarRowKind::Place && r.place == places::PlaceKind::Trash) {
        cmd_entry("cmd.trash.open", CommandId::OpenTrash, 0, "trash.open", true, false);
        cmd_entry("cmd.trash.empty", CommandId::EmptyTrash, 0, "trash.empty",
                  trash_model && trash_model->count() > 0, false);
        view.open_menu(keymap, std::move(entries), x, y);
        return;
    }
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
                leave_trash_view();
                workspace->navigate_active(o.path, o.cursor_name);
            }
            break;
        case ui::PathOutcome::Kind::OpenFile: {
            if (archive::libarchive_available() &&
                archive::is_archive_name(o.path.filename().native()) &&
                !archive::parse_archive_path(o.path.native()).has_value()) {
                leave_trash_view();
                workspace->navigate_active(archive::make_archive_path(o.path, ""));
                break;
            }
            Path open_target = o.path;
            if (const auto arch = archive::parse_archive_path(o.path.native()); arch) {
                const Path tmp_dir =
                    std::filesystem::temp_directory_path() / "navigator-archive-open";
                const Path dest = tmp_dir / Path(arch->inner_path).filename();
                if (auto ex =
                        archive::extract_entry_to_file(arch->archive_file, arch->inner_path, dest);
                    ex) {
                    open_target = dest;
                } else {
                    toast(std::string(tr("notice.archive_failed")), true);
                    break;
                }
            }
            const Result<void> opened =
                launcher ? platform::open_with_default(open_target, *launcher)
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
    const bool trash_now = trash_active();
    if (places->serial() != rows_serial || !(st == rows_state) || folder != rows_folder ||
        language() != rows_language || sidebar_rows.empty() ||
        (trash_model && trash_model->serial() != rows_trash_serial) ||
        trash_now != rows_trash_active) {
        rows_trash.show = true;
        rows_trash.count = trash_model ? trash_model->count() : 0;
        rows_trash.active = trash_now;
        sidebar_rows = ui::build_sidebar_rows(*places, st, folder, ui::kMaxRecentRows, rows_trash);
        rows_trash_serial = trash_model ? trash_model->serial() : 0;
        rows_trash_active = trash_now;
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
    // Phase 6: the Inspector takes list.* while it has the keyboard (docs/keymap.md).
    if (inspector_focus && inspector.visible() && route_inspector(command)) {
        return;
    }
    // Phase 5: commands that move the keyboard elsewhere take it from the filter field.
    if (command.id == CommandId::FocusSidebar || command.id == CommandId::FocusAddressBar ||
        command.id == CommandId::GoTo || command.id == CommandId::FocusNextPane ||
        command.id == CommandId::FocusPreviousPane) {
        filter_tab = layout::TabId{};
    }
    if (command.id == CommandId::ClearSelection && view.dragging()) {
        view.cancel_drag(); // Escape during a tab drag
        return;
    }
    if (command.id == CommandId::ClearSelection && drag.dragging()) {
        drag.cancel(); // Escape during a file drag: nothing happens
        drag_debug_hold = false;
        return;
    }
    // Phase 7: the inline rename editor, sheets and the Trash view take commands first.
    if (file_ops->inline_edit() != nullptr && !view.popup_open()) {
        if (command.id == CommandId::Activate) {
            (void)file_ops->inline_commit(name_taken());
            return;
        }
        if (command.id == CommandId::ClearSelection) {
            file_ops->inline_cancel(); // Escape: nothing is renamed
            return;
        }
        if (is_cursor_command(command.id) || ui::file_command_blocked_while_editing(command.id)) {
            return; // F2, Ctrl+Shift+N, F7 ... never restart or interrupt the editor
        }
    }
    if (file_ops->sheet_open() && command.id != CommandId::Quit &&
        command.id != CommandId::ToggleTheme && command.id != CommandId::ToggleFpsOverlay &&
        command.id != CommandId::CancelOperation && command.id != CommandId::TogglePauseOperation &&
        command.id != CommandId::ToggleOperationsPanel) {
        return; // file commands are not routed while a sheet has the keyboard
    }
    if (is_file_command(command.id)) {
        file_command(command);
        return;
    }
    if (trash_active() && trash_command(command)) {
        return;
    }
    switch (command.id) {
    case CommandId::None: return;
    case CommandId::Quit: quit_request(); return;
    case CommandId::ToggleFpsOverlay:
        show_fps = !show_fps;
        stats_reset = true;
        return;
    case CommandId::ToggleTheme: {
        Theme cur_base = theme;
        previous_palette = resolve_palette(appearance_settings.theme_id, cur_base);
        previous_theme = theme;
        theme = ui::toggled(theme);
        appearance_settings.theme_id = (theme == Theme::Light)
                                           ? std::string(appearance::kBuiltinLightId)
                                           : std::string(appearance::kBuiltinDarkId);
        appearance_dirty = true;
        fade_start_ms = animations ? now_ms() : -1.0;
        return;
    }
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
    case CommandId::ToggleViewLock: {
        ui::BrowserModel& m = workspace->active_model();
        if (!m.current_dir().empty()) {
            const view::ViewScope next =
                view_settings.cycle_lock(m.current_dir(), m.view_settings());
            const view::ResolvedViewSettings res = view_settings.resolve(m.current_dir());
            m.set_view_scope(res.scope, res.rule_path);
            view_settings_dirty = true;
            const char* key = next == view::ViewScope::Folder      ? "view.lock.folder"
                              : next == view::ViewScope::Hierarchy ? "view.lock.hierarchy"
                                                                   : "view.lock.global";
            toast(std::string(tr(key)));
        }
        return;
    }
    case CommandId::ViewLockFolder:
    case CommandId::ViewLockHierarchy:
    case CommandId::ViewLockGlobal: {
        ui::BrowserModel& m = workspace->active_model();
        if (!m.current_dir().empty()) {
            const view::ViewScope scope =
                command.id == CommandId::ViewLockFolder      ? view::ViewScope::Folder
                : command.id == CommandId::ViewLockHierarchy ? view::ViewScope::Hierarchy
                                                             : view::ViewScope::Global;
            view_settings.set_lock(m.current_dir(), scope, m.view_settings());
            const view::ResolvedViewSettings res = view_settings.resolve(m.current_dir());
            m.apply_view_settings(res.settings, res.scope, res.rule_path);
            view_settings_dirty = true;
            const char* key = scope == view::ViewScope::Folder      ? "view.lock.folder"
                              : scope == view::ViewScope::Hierarchy ? "view.lock.hierarchy"
                                                                    : "view.lock.global";
            toast(std::string(tr(key)));
        }
        return;
    }
    case CommandId::ViewModeDetails:
    case CommandId::ViewModeIcons:
    case CommandId::ViewModeList:
    case CommandId::ViewModeTree:
    case CommandId::ViewModeColumns:
    case CommandId::ViewCycleMode: {
        ui::BrowserModel& m = workspace->active_model();
        const view::ViewMode next =
            command.id == CommandId::ViewModeDetails   ? view::ViewMode::Details
            : command.id == CommandId::ViewModeIcons   ? view::ViewMode::Icons
            : command.id == CommandId::ViewModeList    ? view::ViewMode::List
            : command.id == CommandId::ViewModeTree    ? view::ViewMode::Tree
            : command.id == CommandId::ViewModeColumns ? view::ViewMode::Columns
                                                       : view::next_view_mode(m.view_mode());
        m.set_view_mode(next);
        persist_view_change(m);
        return;
    }
    case CommandId::ViewZoomIn:
    case CommandId::ViewZoomOut:
    case CommandId::ViewZoomReset: {
        ui::BrowserModel& m = workspace->active_model();
        if (command.id == CommandId::ViewZoomReset) {
            m.set_zoom_percent(view::kDefaultZoomPercent);
        } else {
            m.step_zoom(command.id == CommandId::ViewZoomIn ? 1 : -1);
        }
        persist_view_change(m);
        return;
    }
    case CommandId::GroupByNone:
    case CommandId::GroupByDate:
    case CommandId::GroupByType:
    case CommandId::GroupBySize:
    case CommandId::GroupCycle: {
        ui::BrowserModel& m = workspace->active_model();
        const view::GroupBy next = command.id == CommandId::GroupByNone   ? view::GroupBy::None
                                   : command.id == CommandId::GroupByDate ? view::GroupBy::Date
                                   : command.id == CommandId::GroupByType ? view::GroupBy::Type
                                   : command.id == CommandId::GroupBySize
                                       ? view::GroupBy::Size
                                       : view::next_group_by(m.group_by());
        m.set_group_by(next);
        persist_view_change(m);
        return;
    }
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
    // Phase 5.
    case CommandId::FocusFilter: focus_filter(true); return;
    case CommandId::ToggleTypesPopup:
        if (view.types_popup_open()) {
            view.close_popup();
        } else {
            view.open_types_popup(*workspace);
        }
        return;
    case CommandId::FlattenVolume: flatten_volume(); return;
    // Phase 6: the Inspector.
    case CommandId::Inspect:
    case CommandId::ToggleInspectorLock:
    case CommandId::InspectorZoomFit:
    case CommandId::InspectorZoom1to1:
    case CommandId::InspectorPlay:
    case CommandId::CycleInspectorPlacement:
    case CommandId::ToggleInspectorGrid:
    case CommandId::ToggleInspectorMinimap: inspector_command(command); return;
    case CommandId::Refresh:
        // F5 re-decodes what the Inspector and the visible thumbnails show.
        if (previews) {
            const ui::BrowserModel& m = workspace->active_model();
            if (m.loaded()) {
                previews->invalidate(m.current_dir(), preview::InvalidateMode::Memory);
                thumbs.forget(m.current_dir());
            }
            if (const ui::inspector::Item* it = inspector.item(); it != nullptr) {
                previews->invalidate(it->path, preview::InvalidateMode::Memory);
                inspector.invalidate();
            }
        }
        (void)workspace->execute(command);
        return;
    case CommandId::RebuildIndex: rebuild_index(); return;
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

// ---------------------------------------------------------------------------------------------
// Phase 7: file commands
// ---------------------------------------------------------------------------------------------

bool App::Impl::is_file_command(CommandId id) noexcept {
    switch (id) {
    case CommandId::Copy:
    case CommandId::Cut:
    case CommandId::Paste:
    case CommandId::Undo:
    case CommandId::Redo:
    case CommandId::MoveToTrash:
    case CommandId::DeletePermanently:
    case CommandId::Rename:
    case CommandId::NewFolder:
    case CommandId::CopyToOtherPane:
    case CommandId::MoveToOtherPane:
    case CommandId::OpenTerminalHere:
    case CommandId::CancelOperation:
    case CommandId::TogglePauseOperation:
    case CommandId::ToggleOperationsPanel:
    case CommandId::OpenTrash:
    case CommandId::RestoreFromTrash:
    case CommandId::EmptyTrash:
    case CommandId::OpenPalette:
    case CommandId::OpenSettings:
    case CommandId::EditKeymap:
    case CommandId::ResetKeymap:
    case CommandId::ExportKeymap:
    case CommandId::ShowContextMenu:
    case CommandId::OpenWith:
    case CommandId::BatchRename:
    case CommandId::ToggleAnimations:
    case CommandId::ToggleAlternatingRows:
    case CommandId::CycleDensity:
    case CommandId::FontSizeIncrease:
    case CommandId::FontSizeDecrease:
    case CommandId::FontSizeReset:
    case CommandId::OpenAppearanceSettings: return true;
    default: return false;
    }
}

bool App::Impl::trash_active() const {
    return workspace && trash_tabs.contains(workspace->active_tab().value);
}

void App::Impl::enter_trash_view() {
    focus_sidebar(false);
    view.close_address_bar();
    filter_tab = layout::TabId{};
    file_ops->inline_cancel();
    trash_tabs.insert(workspace->active_tab().value);
    trash_model->refresh();
}

void App::Impl::leave_trash_view() {
    if (workspace) {
        trash_tabs.erase(workspace->active_tab().value);
    }
}

ui::FileTarget App::Impl::file_target() const {
    ui::FileTarget t;
    t.tab = workspace->active_tab();
    const ui::BrowserModel* m = workspace->model(t.tab);
    if (m == nullptr || !m->loaded()) {
        return t;
    }
    t.dir = m->current_dir();
    const std::size_t n = m->row_count();
    for (std::size_t i = 0; i < n; ++i) {
        if (m->is_selected(i)) {
            t.selected.push_back(m->row_path(i));
        }
    }
    if (const std::size_t c = m->cursor(); c < n) {
        t.cursor = m->row_path(c);
        t.cursor_is_dir = fs::is_directory_like(m->row_entry(c));
    }
    return t;
}

std::function<bool(std::string_view)> App::Impl::name_taken() const {
    const ui::InlineEdit* ie = file_ops->inline_edit();
    const ui::BrowserModel* m = ie != nullptr ? workspace->model(ie->tab) : nullptr;
    return [m](std::string_view name) { return m != nullptr && m->find(name) != ui::kNoRow; };
}

void App::Impl::begin_inline_rename() {
    ui::BrowserModel& m = workspace->active_model();
    if (!m.loaded()) {
        return;
    }
    if (m.flattened()) {
        file_ops->notify(std::string(tr("notice.rename_in_search")));
        return;
    }
    const std::size_t row = m.cursor();
    if (row >= m.row_count()) {
        file_ops->notify(std::string(tr("notice.nothing_selected")));
        return;
    }
    const fs::DirEntry& e = m.row_entry(row);
    focus_sidebar(false);
    view.close_address_bar();
    filter_tab = layout::TabId{};
    file_ops->begin_rename(workspace->active_tab(), m.current_dir(), e.name,
                           fs::is_directory_like(e));
    file_ops->inline_edited(name_taken());
}

void App::Impl::file_command(const Command& command) {
    dirty = true;
    const bool in_trash = trash_active();
    const bool selection_cmd =
        command.id == CommandId::Copy || command.id == CommandId::Cut ||
        command.id == CommandId::Paste || command.id == CommandId::MoveToTrash ||
        command.id == CommandId::DeletePermanently || command.id == CommandId::Rename ||
        command.id == CommandId::NewFolder || command.id == CommandId::CopyToOtherPane ||
        command.id == CommandId::MoveToOtherPane;
    // Focus scoping (docs/keymap.md "Focus scoping (Phase 7)"): no file command acts on a
    // selection that does not have the keyboard. file.rename on a bookmark row renames it.
    if (selection_cmd && (sidebar_focus || view.address_open() || view.popup_open())) {
        if (command.id == CommandId::Rename && sidebar_focus) {
            bookmark_command(Command{CommandId::RenameBookmark, 0});
        }
        return;
    }
    const auto not_in_trash = [&] { file_ops->notify(std::string(tr("notice.trash_view_only"))); };
    switch (command.id) {
    case CommandId::Copy:
    case CommandId::Cut:
        if (in_trash) {
            not_in_trash();
        } else {
            file_ops->copy(file_target(), command.id == CommandId::Cut);
        }
        return;
    case CommandId::Paste:
        if (in_trash) {
            not_in_trash();
        } else {
            file_ops->paste(file_target(), (command.arg & PasteArg::IntoCursorFolder) != 0U);
        }
        return;
    case CommandId::MoveToTrash:
        if (in_trash) {
            // In the Trash view Delete means "delete permanently" and always asks.
            file_ops->request_delete_trashed(trash_model->selected_items());
        } else {
            file_ops->trash(file_target());
        }
        return;
    case CommandId::DeletePermanently:
        if (in_trash) {
            file_ops->request_delete_trashed(trash_model->selected_items());
        } else {
            file_ops->request_delete_permanently(file_target());
        }
        return;
    case CommandId::Rename:
        if (in_trash) {
            not_in_trash();
        } else if (workspace->active_model().selected_count() > 1) {
            open_batch_rename_overlay();
        } else {
            begin_inline_rename();
        }
        return;
    case CommandId::BatchRename:
        if (in_trash) {
            not_in_trash();
        } else {
            open_batch_rename_overlay();
        }
        return;
    case CommandId::NewFolder:
        if (in_trash) {
            not_in_trash();
        } else {
            filter_tab = layout::TabId{};
            file_ops->new_folder(file_target());
        }
        return;
    case CommandId::CopyToOtherPane:
    case CommandId::MoveToOtherPane: {
        if (in_trash) {
            not_in_trash();
            return;
        }
        const std::vector<layout::PaneId> order = workspace->layout().pane_order();
        if (order.size() < 2) {
            file_ops->notify(std::string(tr("notice.one_pane")));
            return;
        }
        std::size_t at = 0;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i] == workspace->active_pane()) {
                at = i;
            }
        }
        const layout::PaneNode* node =
            workspace->layout().find_pane(order[(at + 1) % order.size()]);
        const ui::BrowserModel* other = nullptr;
        if (node != nullptr && node->active_tab < node->tabs.size()) {
            other = workspace->model(node->tabs[node->active_tab].id);
        }
        if (other == nullptr || !other->loaded() ||
            trash_tabs.contains(node->tabs[node->active_tab].id.value)) {
            file_ops->notify(std::string(tr("notice.one_pane")));
            return;
        }
        file_ops->copy_to_folder(file_target(), other->current_dir(),
                                 command.id == CommandId::MoveToOtherPane);
        return;
    }
    case CommandId::Undo: file_ops->undo(); return;
    case CommandId::Redo: file_ops->redo(); return;
    case CommandId::CancelOperation: file_ops->cancel(command.arg); return;
    case CommandId::TogglePauseOperation: file_ops->toggle_pause(command.arg); return;
    case CommandId::ToggleOperationsPanel: file_ops->toggle_panel(); return;
    case CommandId::OpenTerminalHere: {
        const Path dir = workspace->active_model().current_dir();
        const Result<void> res =
            launcher ? platform::open_terminal(dir, *launcher) : platform::open_terminal(dir);
        if (!res) {
            toast(std::string(tr("notice.terminal_failed")), true);
        }
        return;
    }
    case CommandId::ContentSearch: {
        if (content_search_overlay.open()) {
            content_search_overlay.close();
        } else {
            content_search_overlay.open(workspace->active_model().current_dir());
        }
        return;
    }
    case CommandId::ArchiveExtractHere: {
        ui::BrowserModel& m = workspace->active_model();
        if (m.cursor() == ui::kNoRow || m.cursor() >= m.row_count()) {
            return;
        }
        const Path target = m.row_path(m.cursor());
        if (!archive::is_archive_name(target.filename().native())) {
            return;
        }
        const Path dest = m.current_dir() / target.stem();
        if (auto res = archive::extract_entries_to_dir(target, {}, dest); res) {
            toast(std::string(tr("notice.archive_extracted")));
            m.refresh();
        } else {
            toast(std::string(tr("notice.archive_failed")), true);
        }
        return;
    }
    case CommandId::ReloadPlugins: {
        plugin_manager.unload_all();
        const std::size_t n = plugin_manager.load_directory(plugin_dir);
        toast(std::string(tr("notice.plugins_reloaded")) + " (" + std::to_string(n) + ")");
        return;
    }
    case CommandId::UnmountVolume: {
        if (gvfs_backend && !gvfs_mounts.empty()) {
            const std::string uri = gvfs_mounts.front().uri;
            if (auto r = gvfs_backend->unmount(uri); r) {
                toast(std::string(tr("notice.unmounted")));
                if (auto mounts = gvfs_backend->list_mounts(); mounts) {
                    gvfs_mounts = std::move(*mounts);
                }
            }
        }
        return;
    }
    case CommandId::OpenTrash: enter_trash_view(); return;
    case CommandId::RestoreFromTrash:
        if (in_trash) {
            file_ops->restore(trash_model->selected_or_cursor());
        } else {
            file_ops->notify(std::string(tr("notice.nothing_selected")));
        }
        return;
    case CommandId::EmptyTrash:
        if (!trash_model->available()) {
            file_ops->notify(std::string(tr("trash.unavailable")), true);
        } else {
            file_ops->request_empty_trash(trash_model->items());
        }
        return;
    // Phase 8: command palette, settings/keymap editor, searchable context menu, Open With.
    case CommandId::OpenPalette:
        if (!file_ops->sheet_open()) {
            if (palette_overlay.is_open()) {
                palette_overlay.close();
            } else {
                open_palette_overlay();
            }
        }
        return;
    case CommandId::OpenSettings:
    case CommandId::EditKeymap:
    case CommandId::OpenAppearanceSettings:
        if (!file_ops->sheet_open()) {
            const ui::KeymapSettingsOverlay::Page target_page =
                command.id == CommandId::EditKeymap ? ui::KeymapSettingsOverlay::Page::Keyboard
                                                    : ui::KeymapSettingsOverlay::Page::Appearance;
            if (keymap_settings.is_open() &&
                (command.id == CommandId::OpenSettings || keymap_settings.page() == target_page)) {
                keymap_settings.close();
            } else {
                seq_matcher.reset();
                seq_status_text.clear();
                palette_overlay.close();
                context_menu.close();
                openwith_overlay.close();
                batch_rename_overlay.close();
                keymap_settings.open(registry, keymap, target_page);
            }
        }
        return;
    case CommandId::ToggleAnimations:
        appearance_settings.animations = !appearance_settings.animations;
        apply_appearance_settings();
        appearance_dirty = true;
        toast(std::string(tr("settings.appearance.animations")) + ": " +
              std::string(tr(appearance_settings.animations ? "settings.on" : "settings.off")));
        return;
    case CommandId::ToggleAlternatingRows:
        appearance_settings.alternating_rows = !appearance_settings.alternating_rows;
        appearance_dirty = true;
        dirty = true;
        toast(
            std::string(tr("settings.appearance.alternating_rows")) + ": " +
            std::string(tr(appearance_settings.alternating_rows ? "settings.on" : "settings.off")));
        return;
    case CommandId::CycleDensity:
        appearance_settings.density = appearance_settings.density == appearance::UiDensity::Compact
                                          ? appearance::UiDensity::Comfortable
                                          : appearance::UiDensity::Compact;
        apply_appearance_settings();
        appearance_dirty = true;
        toast(std::string(tr(appearance_settings.density == appearance::UiDensity::Compact
                                 ? "settings.appearance.compact"
                                 : "settings.appearance.comfortable")));
        return;
    case CommandId::FontSizeIncrease:
    case CommandId::FontSizeDecrease:
    case CommandId::FontSizeReset:
        if (command.id == CommandId::FontSizeReset) {
            appearance_settings.font_scale_percent = appearance::kDefaultFontScalePercent;
        } else {
            const int delta = command.id == CommandId::FontSizeIncrease
                                  ? appearance::kFontScaleStepPercent
                                  : -appearance::kFontScaleStepPercent;
            appearance_settings.font_scale_percent =
                appearance::clamp_font_scale(appearance_settings.font_scale_percent + delta);
        }
        appearance_dirty = true;
        dirty = true;
        toast(std::string(tr("settings.appearance.font_size")) + ": " +
              std::to_string(appearance_settings.font_scale_percent) + "%");
        return;
    case CommandId::ResetKeymap:
        if (!file_ops->sheet_open()) {
            seq_matcher.reset();
            seq_status_text.clear();
            palette_overlay.close();
            context_menu.close();
            keymap_settings.close();
            reset_keymap_confirm = true;
            reset_confirm_focus = 0; // safe default: Cancel
        }
        return;
    case CommandId::ExportKeymap:
        set_clipboard(to_json(keymap_file));
        toast(std::string(tr("keymap.export_done")));
        return;
    case CommandId::ShowContextMenu:
        if (!file_ops->sheet_open() && text_field() == nullptr && !filter_focused()) {
            menu::Target target;
            menu::MenuKind kind = menu::MenuKind::ListBackground;
            if (in_trash) {
                kind = trash_model->selected_or_cursor().empty() ? menu::MenuKind::TrashBackground
                                                                 : menu::MenuKind::TrashRow;
            } else if (sidebar_focus && sidebar_row < sidebar_rows.size()) {
                const ui::SidebarRow& sr = sidebar_rows[sidebar_row];
                if (sr.kind == ui::SidebarRowKind::Bookmark) {
                    kind = menu::MenuKind::SidebarBookmark;
                    target.bookmark = sr.bookmark.value;
                } else {
                    kind = menu::MenuKind::SidebarPlace;
                }
            } else {
                const ui::BrowserModel& m = workspace->active_model();
                kind = (m.cursor_entry() != nullptr) ? menu::MenuKind::ListRow
                                                     : menu::MenuKind::ListBackground;
            }
            const float ax = std::max(40.0F, window_w * 0.35F);
            const float ay = std::max(80.0F, window_h * 0.30F);
            open_context_menu_at(kind, ax, ay, target);
        }
        return;
    case CommandId::OpenWith:
        if (!file_ops->sheet_open() && !in_trash) {
            open_openwith_overlay();
        }
        return;
    default: return;
    }
}

bool App::Impl::trash_command(const Command& command) {
    const bool extend = (command.arg & CursorArg::Extend) != 0U;
    switch (command.id) {
    case CommandId::CursorUp: trash_model->cursor_by(-1, extend); return true;
    case CommandId::CursorDown: trash_model->cursor_by(1, extend); return true;
    case CommandId::CursorPageUp:
        trash_model->cursor_page(-1, extend, trash_view.page_rows());
        return true;
    case CommandId::CursorPageDown:
        trash_model->cursor_page(1, extend, trash_view.page_rows());
        return true;
    case CommandId::CursorHome: trash_model->cursor_home(extend); return true;
    case CommandId::CursorEnd: trash_model->cursor_end(extend); return true;
    case CommandId::SelectAll: trash_model->select_all(); return true;
    case CommandId::ClearSelection: trash_model->clear_selection(); return true;
    case CommandId::Activate:
        file_ops->restore(trash_model->selected_or_cursor()); // Enter restores; never deletes
        return true;
    case CommandId::NavigateBack:
    case CommandId::NavigateUp: leave_trash_view(); return true;
    case CommandId::NavigateForward: return true;
    case CommandId::Refresh: trash_model->refresh(); return true;
    case CommandId::FocusFilter:
    case CommandId::ToggleFlatten:
    case CommandId::FlattenVolume:
    case CommandId::ToggleTypesPopup:
    case CommandId::OpenInNewTab: return true; // nothing to filter or open in the Trash
    default: return false;
    }
}

void App::Impl::quit_request() {
    if (file_ops->request_quit()) {
        running = false;
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 8: keymap layering, CommandContext, Palette, Context Menu, Open With
// ---------------------------------------------------------------------------------------------

void App::Impl::rebuild_keymap_from_file() {
    if (auto km = build_keymap(keymap_file, registry, nullptr); km) {
        keymap = std::move(*km);
    } else {
        keymap = default_keymap;
    }
    seq_matcher.set_timeout(keymap_file.sequence_timeout);
    seq_matcher.reset();
    seq_status_text.clear();
    keymap_settings.refresh(registry, keymap);
}

CommandContext App::Impl::capture_context() const {
    CommandContext ctx;
    const bool in_trash = workspace && trash_tabs.contains(workspace->active_tab().value);
    if (in_trash) {
        ctx.flags |= ContextFlags::TrashView;
        if (trash_model && !trash_model->selected_or_cursor().empty()) {
            ctx.flags |= ContextFlags::HasSelection | ContextFlags::HasCursorRow;
        }
    } else if (workspace) {
        const ui::BrowserModel& m = workspace->active_model();
        if (m.selected_count() > 0) {
            ctx.flags |= ContextFlags::HasSelection;
        }
        if (const fs::DirEntry* cur = m.cursor_entry(); cur != nullptr) {
            ctx.flags |= ContextFlags::HasCursorRow;
            if (fs::is_directory_like(*cur)) {
                ctx.flags |= ContextFlags::CursorOnFolder;
            }
        }
        if (m.flattened()) {
            ctx.flags |= ContextFlags::FlattenedView;
        }
        if (!m.filter_edit().text().empty()) {
            ctx.flags |= ContextFlags::FilterActive;
        }
        if (m.flatten_progress().walking) {
            ctx.flags |= ContextFlags::SearchRunning;
        }
        if (m.can_back()) {
            ctx.flags |= ContextFlags::CanGoBack;
        }
        if (m.can_forward()) {
            ctx.flags |= ContextFlags::CanGoForward;
        }
    }
    if (sidebar_focus) {
        ctx.flags |= ContextFlags::SidebarFocused;
        if (sidebar_row < sidebar_rows.size() &&
            sidebar_rows[sidebar_row].kind == ui::SidebarRowKind::Bookmark) {
            ctx.flags |= ContextFlags::BookmarkRowFocused;
        }
    }
    if (workspace && workspace->layout().pane_count() > 1U) {
        ctx.flags |= ContextFlags::MultiplePanes;
    }
    if (workspace && !workspace->closed_tabs().empty()) {
        ctx.flags |= ContextFlags::ClosedTabsAvailable;
    }
    if (file_ops) {
        const auto run_count = static_cast<std::uint32_t>(file_ops->running().size());
        ctx.running_operations = run_count;
        if (run_count > 0) {
            ctx.flags |= ContextFlags::OperationsRunning;
        }
        if (file_ops->undo_stack().can_undo()) {
            ctx.flags |= ContextFlags::CanUndo;
        }
        if (file_ops->undo_stack().can_redo()) {
            ctx.flags |= ContextFlags::CanRedo;
        }
    }
    if (inspector.visible()) {
        ctx.flags |= ContextFlags::InspectorVisible;
        if (const auto& res = inspector.result();
            res != nullptr && res->kind() == preview::PreviewKind::Image) {
            ctx.flags |= ContextFlags::InspectorShowsImage;
        }
    }
    return ctx;
}

void App::Impl::open_palette_overlay() {
    const CommandContext ctx = capture_context();
    seq_matcher.reset();
    seq_status_text.clear();
    context_menu.close();
    keymap_settings.close();
    openwith_overlay.close();

    std::string undo_lbl;
    std::string redo_lbl;
    std::vector<ui::PickerOption> ops_opts;
    if (file_ops) {
        if (const auto u = file_ops->undo_text()) {
            undo_lbl = *u;
        }
        if (const auto r = file_ops->redo_text()) {
            redo_lbl = *r;
        }
        for (const auto& op : file_ops->running()) {
            ops_opts.push_back(ui::PickerOption{op.id.value, op.current.filename().native(), {}});
        }
    }
    std::vector<ui::PickerOption> bm_opts;
    if (places) {
        for (const places::Bookmark& b : places->bookmarks().list()) {
            const std::string lbl = b.name.empty() ? b.path.filename().native() : b.name;
            bm_opts.push_back(ui::PickerOption{b.id.value, lbl, b.path.native()});
        }
    }
    const std::vector<Alias> aliases = effective_aliases(keymap_file, registry, nullptr);
    palette_overlay.open(registry, keymap, aliases, palette_history, ctx, undo_lbl, redo_lbl,
                         std::move(ops_opts), std::move(bm_opts));
}

void App::Impl::open_context_menu_at(menu::MenuKind kind, float x, float y,
                                     const menu::Target& target) {
    const CommandContext ctx = capture_context();
    seq_matcher.reset();
    seq_status_text.clear();
    palette_overlay.close();
    keymap_settings.close();
    openwith_overlay.close();

    openwith_targets.clear();
    openwith_handlers.clear();
    const bool in_trash = workspace && trash_tabs.contains(workspace->active_tab().value);
    if (kind == menu::MenuKind::ListRow && !in_trash) {
        const ui::FileTarget ft = file_target();
        if (!ft.selected.empty()) {
            openwith_targets = ft.selected;
        } else if (!ft.cursor.empty()) {
            openwith_targets.push_back(ft.cursor);
        }
    }
    context_menu.open(kind, registry, keymap, ctx, target, menu_pins, x, y,
                      !openwith_targets.empty());
    if (!openwith_targets.empty()) {
        if (const auto mime = platform::mime_type(openwith_targets.front()); mime) {
            if (auto list = openwith::list_handlers(*mime, openwith::system_options()); list) {
                openwith_handlers = std::move(*list);
            }
        }
        context_menu.set_open_with_handlers(openwith_handlers);
    }
}

void App::Impl::open_openwith_overlay() {
    openwith_targets.clear();
    const ui::FileTarget ft = file_target();
    if (!ft.selected.empty()) {
        openwith_targets = ft.selected;
    } else if (!ft.cursor.empty()) {
        openwith_targets.push_back(ft.cursor);
    }
    if (openwith_targets.empty()) {
        toast(std::string(tr("notice.nothing_selected")));
        return;
    }
    seq_matcher.reset();
    seq_status_text.clear();
    palette_overlay.close();
    context_menu.close();
    keymap_settings.close();

    std::string primary_mime;
    if (const auto mime = platform::mime_type(openwith_targets.front()); mime) {
        primary_mime = *mime;
    }
    openwith_overlay.open(openwith_targets, primary_mime);
    openwith_handlers.clear();
    if (!primary_mime.empty()) {
        if (auto list = openwith::list_handlers(primary_mime, openwith::system_options()); list) {
            openwith_handlers = std::move(*list);
        }
    }
    openwith_overlay.set_handlers(openwith_handlers);
}

void App::Impl::launch_openwith_handler(const openwith::Handler& h, const std::vector<Path>& paths,
                                        std::string_view mime, bool set_default) {
    if (set_default && !mime.empty()) {
        (void)openwith::set_default_handler(mime, h.desktop_id, openwith::system_options());
    }
    openwith::LaunchOptions lopt;
    if (h.terminal) {
        lopt.expand.terminal_prefix = openwith::default_terminal_prefix(
            [](std::string_view prog) { return platform::find_program(prog).has_value(); });
    }
    if (launcher) {
        if (auto res = openwith::launch_with(h, paths, *launcher, lopt); !res) {
            toast(res.error().message, true);
        }
    }
}

void App::Impl::apply_palette_action(const ui::PaletteAction& act) {
    if (act.record_history && !act.history_key.empty()) {
        palette_history.record(act.history_key);
        history_dirty = true;
    }
    queue.enqueue(act.command);
}

void App::Impl::apply_menu_action(const ui::MenuAction& act) {
    if (act.kind == ui::MenuAction::Kind::Command) {
        queue.enqueue(act.command);
    } else if (act.kind == ui::MenuAction::Kind::TogglePin) {
        if (menu_pins.contains(act.pin_id)) {
            (void)menu_pins.unpin(act.pin_id);
        } else {
            (void)menu_pins.pin(act.pin_id);
        }
        pins_dirty = true;
        context_menu.reapply_pins(menu_pins);
    } else if (act.kind == ui::MenuAction::Kind::OpenWith) {
        for (const openwith::Handler& h : openwith_handlers) {
            if (h.desktop_id == act.handler_id) {
                launch_openwith_handler(h, openwith_targets, {}, false);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 7: drag and drop
// ---------------------------------------------------------------------------------------------

void App::Impl::drag_begin(std::uint32_t tab, std::size_t row) {
    ui::BrowserModel* m = workspace->model(layout::TabId{tab});
    if (m == nullptr || row >= m->row_count() || trash_tabs.contains(tab) ||
        file_ops->sheet_open() || file_ops->inline_edit() != nullptr) {
        return;
    }
    if (!m->is_selected(row)) {
        m->click_row(row, Modifiers::None); // dragging an unselected row drags just that row
    }
    std::vector<Path> items;
    bool have_first = false;
    for (std::size_t i = 0; i < m->row_count(); ++i) {
        if (m->is_selected(i)) {
            if (!have_first) {
                drag_first = m->row_entry(i);
                have_first = true;
            }
            items.push_back(m->row_path(i));
        }
    }
    if (items.empty()) {
        return;
    }
    if (env("NAVIGATOR_DEBUG_DND") != nullptr) {
        SDL_Log("Navigator: drag begins with %zu item(s)", items.size());
    }
    drag.begin(std::move(items), tab, now_ms());
    outgoing_tried = false;
    outgoing_live = false;
}

void App::Impl::apply_drop(std::vector<Path> items, const ui::DropEval& eval, float x, float y) {
    if (items.empty()) {
        return;
    }
    if (env("NAVIGATOR_DEBUG_DND") != nullptr) {
        SDL_Log("Navigator: drop of %zu item(s): action %d trash %d folder %s", items.size(),
                static_cast<int>(eval.action), eval.trash ? 1 : 0, eval.folder.c_str());
    }
    if (eval.trash) {
        file_ops->trash_paths(std::move(items));
        return;
    }
    switch (eval.action) {
    case transfer::DropAction::None: return;
    case transfer::DropAction::Ask: {
        pending_drop = PendingDrop{std::move(items), eval.folder};
        using Entry = ui::WorkspaceView::MenuEntry;
        std::vector<Entry> entries;
        const auto add = [&](std::string_view key, transfer::DropAction a) {
            Entry e;
            e.label = std::string(tr(key));
            ui::Intent in;
            in.kind = ui::Intent::Kind::DropChoose;
            in.index = static_cast<std::size_t>(a);
            e.intent = in;
            entries.push_back(std::move(e));
        };
        add("drag.copy", transfer::DropAction::Copy);
        add("drag.move", transfer::DropAction::Move);
        add("drag.link", transfer::DropAction::Link);
        Entry cancel;
        cancel.label = std::string(tr("confirm.cancel"));
        cancel.separator_before = true;
        entries.push_back(std::move(cancel));
        view.open_menu(keymap, std::move(entries), x, y);
        return;
    }
    case transfer::DropAction::Copy:
    case transfer::DropAction::Move:
    case transfer::DropAction::Link:
        file_ops->transfer(std::move(items), eval.folder, eval.action);
        return;
    }
}

void App::Impl::drag_frame_end(double t_ms) {
    if (!drag.dragging()) {
        return;
    }
    if (!drag_debug_hold && !pointer.down && !pointer.released) {
        drag.cancel(); // the button went up without us seeing it
        return;
    }
    drag.update(pointer.x, pointer.y, pointer.released ? release_mods : pointer.mods, slots, t_ms,
                ui::default_drop_probe());
    // Outgoing: the drag left the window, so other programs may take it (docs/dnd.md).
    const bool in_window =
        pointer.x >= 0.0F && pointer.y >= 0.0F && pointer.x <= window_w && pointer.y <= window_h;
    if (outgoing && !drag_debug_hold) {
        if (!in_window) {
            if (!outgoing_tried && !outgoing->active()) {
                outgoing_tried = true;
                outgoing_live = outgoing->begin(drag.items());
                if (env("NAVIGATOR_DEBUG_DND") != nullptr) {
                    SDL_Log("Navigator: outgoing drag %s (%s)",
                            outgoing_live ? "started" : "refused", outgoing->state().c_str());
                }
            }
            if (outgoing_live && outgoing->active()) {
                (void)outgoing->motion();
            }
        } else if (outgoing_live && outgoing->active()) {
            outgoing->finish(false); // back over Navigator: the internal drag continues
            outgoing_live = false;
            outgoing_tried = false;
        }
        if (pointer.released && outgoing_live && outgoing->active()) {
            outgoing->finish(true); // drop on the other program's window
            outgoing_live = false;
            outgoing_tried = false;
            drag.cancel();
            return;
        }
    }
    if (animations && !view.popup_open()) {
        if (const std::optional<ui::DropSlot> spring = drag.take_spring(t_ms); spring) {
            // Spring-loaded folder: it opens in the pane it was hovered in.
            if (ui::BrowserModel* tm = workspace->model(layout::TabId{spring->tab});
                tm != nullptr) {
                tm->navigate_to(spring->folder);
            }
        }
    }
    if (pointer.released && !drag_debug_hold) {
        ui::FileDrag::Outcome o = drag.finish();
        if (o.kind == ui::FileDrag::Outcome::Kind::Drop) {
            apply_drop(std::move(o.items), o.eval, pointer.x, pointer.y);
        }
    }
}

void App::Impl::incoming_update() {
    if (!incoming.active() || !incoming.has_position()) {
        return;
    }
    const ui::DropSlot* slot = ui::pick_slot(slots, incoming.x(), incoming.y());
    const Modifiers mods = ui::translate_modifiers(SDL_GetModState());
    if (incoming.paths().empty()) {
        // SDL delivers the paths when the drop happens; until then the highlight shows where a
        // copy (or a move with Shift) would land.
        incoming_eval = ui::DropEval{};
        if (slot != nullptr && slot->kind != ui::SlotKind::Trash && !slot->folder.empty()) {
            incoming_eval.kind = slot->kind;
            incoming_eval.folder = slot->folder;
            incoming_eval.label = slot->label;
            incoming_eval.rect = slot->rect;
            incoming_eval.action = has(mods, Modifiers::Alt)     ? transfer::DropAction::Ask
                                   : has(mods, Modifiers::Shift) ? transfer::DropAction::Move
                                                                 : transfer::DropAction::Copy;
        }
        incoming_first = fs::DirEntry{};
        return;
    }
    incoming_eval =
        ui::evaluate_drop(incoming.paths(), slot, false, mods, ui::default_drop_probe());
    if (Result<fs::DirEntry> e = fs::stat(incoming.paths().front(), false); e) {
        incoming_first = *e;
    }
}

void App::Impl::handle_drop_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_DROP_BEGIN: incoming.begin(); break;
    case SDL_EVENT_DROP_POSITION:
        pointer.x = event.drop.x * pixel_density;
        pointer.y = event.drop.y * pixel_density;
        pointer.inside = true;
        incoming.position(pointer.x, pointer.y);
        incoming_update();
        break;
    case SDL_EVENT_DROP_FILE:
        if (event.drop.data != nullptr) {
            incoming.add_path(Path(event.drop.data));
        }
        break;
    case SDL_EVENT_DROP_TEXT:
        if (event.drop.data != nullptr) {
            (void)incoming.add_text(event.drop.data);
        }
        break;
    case SDL_EVENT_DROP_COMPLETE: {
        const float x = incoming.has_position() ? incoming.x() : pointer.x;
        const float y = incoming.has_position() ? incoming.y() : pointer.y;
        std::vector<Path> items = incoming.complete();
        incoming_eval = ui::DropEval{};
        if (items.empty()) {
            file_ops->notify(std::string(tr("notice.drop_failed")), true);
            break;
        }
        const ui::DropSlot* slot = ui::pick_slot(slots, x, y);
        const ui::DropEval eval =
            ui::evaluate_drop(items, slot, false, ui::translate_modifiers(SDL_GetModState()),
                              ui::default_drop_probe());
        apply_drop(std::move(items), eval, x, y);
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 7: reconciling views after operations
// ---------------------------------------------------------------------------------------------

void App::Impl::apply_reconcile(double t_ms) {
    ui::Reconcile rc = file_ops->take_reconcile();
    if (rc.empty()) {
        return;
    }
    dirty = true;
    for (const Path& folder : rc.refresh) {
        for (const layout::TabId tab : workspace->visible_tabs()) {
            if (ui::BrowserModel* m = workspace->model(tab);
                m != nullptr && m->loaded() && m->current_dir() == folder) {
                m->refresh();
            }
        }
    }
    // A locked Inspector follows a moved or renamed item.
    for (const auto& [from, to] : rc.moved) {
        if (to.empty() || !inspector.locked() || inspector.item() == nullptr ||
            inspector.item()->path != from) {
            continue;
        }
        if (Result<fs::DirEntry> e = fs::stat(to, false); e) {
            ui::inspector::Item it;
            it.path = to;
            it.entry = *e;
            it.location = location_for(to);
            (void)inspector.retarget(from, std::move(it), t_ms);
        }
    }
    for (ui::Reconcile::Select& sel : rc.select) {
        pending_select.push_back(
            PendingSelect{std::move(sel.dir), std::move(sel.names), t_ms + 3000.0});
    }
    if (rc.rename_after_create) {
        pending_rename =
            PendingRename{rc.rename_after_create->tab, std::move(rc.rename_after_create->dir),
                          std::move(rc.rename_after_create->name), t_ms + 3000.0};
    }
    if (rc.trash_changed && trash_model && trash_model->available()) {
        trash_model->refresh();
    }
}

void App::Impl::process_pending(double t_ms) {
    // The inline editor belongs to one folder of one tab: leaving either commits (GNOME Files).
    if (ui::InlineEdit* ie = file_ops->inline_edit(); ie != nullptr) {
        const ui::BrowserModel* m = workspace->model(ie->tab);
        if (m == nullptr || !m->loaded() || m->current_dir() != ie->dir) {
            file_ops->inline_cancel();
        } else if (workspace->active_tab() != ie->tab) {
            (void)file_ops->inline_commit(name_taken());
        }
    }
    if (pending_rename) {
        ui::BrowserModel* m = workspace->model(pending_rename->tab);
        if (m == nullptr || t_ms > pending_rename->deadline) {
            pending_rename.reset();
        } else if (m->loaded() && m->current_dir() == pending_rename->dir) {
            const std::size_t row = m->find_row(pending_rename->name);
            if (row != ui::kNoRow) {
                m->click_row(row, Modifiers::None);
                focus_sidebar(false);
                view.close_address_bar();
                filter_tab = layout::TabId{};
                file_ops->begin_rename(pending_rename->tab, pending_rename->dir,
                                       pending_rename->name, true, true);
                file_ops->inline_edited(name_taken());
                pending_rename.reset();
                dirty = true;
            }
        }
    }
    for (auto it = pending_select.begin(); it != pending_select.end();) {
        bool done = t_ms > it->deadline;
        for (const layout::TabId tab : workspace->visible_tabs()) {
            ui::BrowserModel* m = workspace->model(tab);
            if (m != nullptr && m->loaded() && m->current_dir() == it->dir && !done) {
                bool all = true;
                for (const std::string& n : it->names) {
                    all = all && m->find_row(n) != ui::kNoRow;
                }
                if (all) {
                    if (tab == workspace->active_tab() || !pending_rename) {
                        (void)m->select_by_names(it->names);
                    }
                    done = true;
                    dirty = true;
                }
            }
        }
        it = done ? pending_select.erase(it) : std::next(it);
    }
    // Trash tabs whose tab closed.
    for (auto it = trash_tabs.begin(); it != trash_tabs.end();) {
        it = workspace->model(layout::TabId{*it}) == nullptr ? trash_tabs.erase(it) : std::next(it);
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
                trash_tabs.erase(in.tab);
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
                leave_trash_view();
                workspace->navigate_active(in.path);
            }
            break;
        case Kind::CopyText:
            set_clipboard(in.text);
            toast(ui::substitute(tr("copy.copied_one"), "path", in.text));
            break;
        case Kind::SetTypeVisible:
            if (m != nullptr) {
                search::TypeFilter tf = m->type_filter();
                search::TypeKey key;
                key.kind = static_cast<search::TypeKind>(std::min<std::size_t>(in.index, 2));
                key.extension = key.kind == search::TypeKind::Extension ? in.text : std::string();
                search::set_visible(tf, key, in.additive);
                m->set_type_filter(std::move(tf));
            }
            break;
        case Kind::ToggleAllTypes:
            if (m != nullptr) {
                search::TypeFilter tf = m->type_filter();
                const std::vector<search::TypeCount> present = m->type_summary();
                search::toggle_all(tf, Span<const search::TypeCount>(present));
                m->set_type_filter(std::move(tf));
            }
            break;
        // Phase 7.
        case Kind::DragBegin: drag_begin(in.tab, in.row); break;
        case Kind::InlineCommit:
            if (file_ops->inline_edit() != nullptr) {
                (void)file_ops->inline_commit(name_taken());
            }
            break;
        case Kind::DismissOpsNotice: file_ops->dismiss_notice(); break;
        case Kind::SheetButton: file_ops->sheet_activate(in.index); break;
        case Kind::SheetApplyAll: file_ops->sheet_toggle_apply_all(); break;
        case Kind::SheetEditName: file_ops->conflict_begin_name_edit(); break;
        case Kind::TrashClickRow: trash_model->click_row(in.row, in.mods); break;
        case Kind::TrashClearSelect: trash_model->clear_selection(); break;
        case Kind::DropChoose:
            if (pending_drop) {
                PendingDrop pd = std::move(*pending_drop);
                pending_drop.reset();
                file_ops->transfer(
                    std::move(pd.items), pd.folder,
                    static_cast<transfer::DropAction>(std::min<std::size_t>(in.index, 3)));
            }
            break;
        case Kind::SetViewMode:
            if (m != nullptr) {
                m->set_view_mode(static_cast<view::ViewMode>(in.index));
                persist_view_change(*m);
            }
            break;
        case Kind::SetZoomPercent:
            if (m != nullptr) {
                if (in.additive && m->view_mode() == view::ViewMode::Details) {
                    m->set_view_mode(view::ViewMode::Icons);
                }
                m->set_zoom_percent(static_cast<int>(in.index));
                persist_view_change(*m);
            }
            break;
        case Kind::StepZoom:
            if (m != nullptr) {
                m->step_zoom(in.position >= 0.0 ? 1 : -1);
                persist_view_change(*m);
            }
            break;
        case Kind::ReorderColumn:
            if (m != nullptr) {
                m->reorder_column(in.row, in.row_last);
                persist_view_change(*m);
            }
            break;
        case Kind::ToggleGroup:
            if (m != nullptr) {
                m->toggle_group_collapsed(static_cast<std::uint8_t>(in.index));
            }
            break;
        case Kind::TreeToggle:
            if (m != nullptr) {
                m->tree_toggle(in.path);
            }
            break;
        case Kind::TreeClickRow:
            if (m != nullptr) {
                m->set_tree_cursor(in.row);
                const auto& tr = m->tree_rows();
                if (in.row < tr.size()) {
                    const ui::TreeRow& row = tr[in.row];
                    if (row.root_row != ui::kTreeNoRootRow && row.root_row < m->row_count()) {
                        m->click_row(row.root_row, in.mods);
                    }
                    if (in.additive) {
                        if (row.expandable) {
                            m->navigate_to(row.full_path);
                        } else if (launcher != nullptr) {
                            (void)platform::open_with_default(row.full_path, *launcher);
                        }
                    }
                }
            }
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
    // Phase 6: the Inspector (smoke tests check what it shows and how long frames took).
    static constexpr const char* kStatus[] = {"empty", "multi", "loading", "ready", "failed"};
    static constexpr const char* kKind[] = {"placeholder", "image", "video", "text", "folder"};
    const layout::InspectorState& st = workspace->inspector();
    out << "inspector " << (inspector.visible() ? "visible" : "hidden") << " placement "
        << (dock.placement == layout::InspectorPlacement::Bottom ? "bottom" : "right") << " status "
        << kStatus[static_cast<std::size_t>(inspector.status())];
    if (inspector.result() && inspector.result_is_current()) {
        out << " kind " << kKind[static_cast<std::size_t>(inspector.result()->kind())];
    }
    if (const ui::inspector::Item* it = inspector.item(); it != nullptr) {
        out << " item " << it->path.filename().string();
    }
    out << " locked " << (inspector.locked() ? 1 : 0) << " grid " << (st.show_grid ? 1 : 0)
        << " minimap " << (st.show_minimap ? 1 : 0) << " zoom " << inspector_view.viewport().zoom()
        << "\n";
    out << "inspector_max_frame_ms " << inspector_max_ms << "\n";
    out << "thumbnails slots " << thumbs.slot_count() << " textures "
        << (textures ? textures->count() : 0) << "\n";
}

void App::Impl::run_script_step() {
    if (script_step >= script.size() || !workspace->settled() || view.paths_busy() ||
        !outcomes.empty() || !queue.empty()) {
        return;
    }
    {
        const std::string& next = script[script_step];
        if (next == "waitsearch" && workspace->searching()) {
            return;
        }
        if (next.starts_with("waitfound:")) {
            const ui::FlattenProgress prog = workspace->active_model().flatten_progress();
            if (prog.walking && prog.found < std::strtoull(next.c_str() + 10, nullptr, 10)) {
                return;
            }
        }
        if (next == "waitpreview" &&
            (inspector.busy() || inspector.status() == ui::inspector::Status::Loading ||
             (textures && textures->uploading()) || inspector_view.animating())) {
            return;
        }
        if (next == "waitthumbs" &&
            (thumbs.pending_count() > 0 || (textures && textures->uploading()))) {
            return;
        }
        if (next == "waitops" &&
            (file_ops->busy() || !pending_select.empty() || pending_rename.has_value() ||
             (trash_model && trash_model->loading()))) {
            return;
        }
        if (next == "waitprogress" &&
            (file_ops->running().empty() || file_ops->running().front().bytes_done == 0)) {
            return;
        }
        if (next == "waitconflict" && file_ops->sheet_kind() != ui::SheetKind::Conflict) {
            return;
        }
        if (next == "waitindex") {
            const auto index = indexes->index_for(workspace->active_model().current_dir());
            if (index && index->status().state != search::IndexState::Ready &&
                index->status().state != search::IndexState::Failed) {
                return;
            }
        }
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
    } else if (op == "palette") {
        open_palette_overlay();
    } else if (op == "ptype") {
        palette_overlay.set_query(arg, palette_history);
    } else if (op == "penter") {
        if (const auto act = palette_overlay.handle_key(KeyChord{Key::Enter, Modifiers::None},
                                                        palette_history)) {
            apply_palette_action(*act);
        }
    } else if (op == "cmenu") {
        queue.enqueue(CommandId::ShowContextMenu);
    } else if (op == "cmtype") {
        context_menu.set_query(arg);
    } else if (op == "cmenter") {
        if (const auto act = context_menu.handle_key(KeyChord{Key::Enter, Modifiers::None})) {
            apply_menu_action(*act);
        }
    } else if (op == "rebind" && parts.size() >= 3) {
        if (const auto seq = parse_key_sequence(parts[2]); seq) {
            if (set_binding(keymap_file, keymap, default_keymap, registry, *seq, arg, 0, true)) {
                keymap_dirty = true;
                rebuild_keymap_from_file();
                flush_phase8_files();
            }
        }
    } else if (op == "seq") {
        for (const std::string& tok : split(arg, ',')) {
            if (const auto kc = parse_key_chord(tok); kc) {
                handle_key(*kc, false);
            }
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
        handle_text(raw.substr(5)); // the same path as SDL text input (type-to-filter too)
    } else if (op == "filter") {
        focus_filter(false);
        model.set_filter_text(raw.substr(7));
    } else if (op == "blurfilter") {
        filter_tab = layout::TabId{};
    } else if (op == "inspect") {
        // Cursor to NAME and show the Inspector (Space).
        const std::size_t row = model.find(arg);
        if (row != ui::kNoRow) {
            model.click_row(row, Modifiers::None);
        }
        if (!inspector.visible()) {
            set_inspector_visible(true);
        }
    } else if (op == "inspectorfocus") {
        inspector_focus = true;
    } else if (op == "hoverplay") {
        inspector_view.debug_hover_play(); // screenshots: the Play button's tooltip
    } else if (op == "waitsearch" || op == "waitfound" || op == "waitindex" ||
               op == "waitpreview" || op == "waitthumbs" || op == "waitops" ||
               op == "waitprogress" || op == "waitconflict") {
        // Held before the step was taken (above).
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
    } else if (op == "panel") {
        if (!file_ops->panel_open()) {
            file_ops->toggle_panel();
        }
    } else if (op == "conflict") {
        // conflict:skip|replace|keep|merge|cancel[:all]
        if (ui::ConflictSheet* cs = file_ops->conflict_sheet(); cs != nullptr) {
            cs->apply_all = parts.size() > 2 && parts[2] == "all";
            const std::vector<ui::ConflictButton> buttons = ui::conflict_buttons(cs->conflict);
            const ui::ConflictButton want = arg == "skip"      ? ui::ConflictButton::Skip
                                            : arg == "replace" ? ui::ConflictButton::Replace
                                            : arg == "merge"   ? ui::ConflictButton::Merge
                                            : arg == "cancel"  ? ui::ConflictButton::Cancel
                                                               : ui::ConflictButton::KeepBoth;
            for (std::size_t i = 0; i < buttons.size(); ++i) {
                if (buttons[i] == want) {
                    file_ops->sheet_activate(i);
                    break;
                }
            }
        }
    } else if (op == "confirm") {
        if (file_ops->confirm_sheet() != nullptr) {
            file_ops->sheet_activate(arg == "yes" ? 1U : 0U);
        }
    } else if (op == "trashclick") {
        for (std::size_t i = 0; i < trash_model->count(); ++i) {
            if (ui::TrashModel::display_name_of(trash_model->items()[i]) == arg) {
                trash_model->click_row(i, parts.size() > 2 &&
                                                  parts[2].find("ctrl") != std::string::npos
                                              ? Modifiers::Ctrl
                                              : Modifiers::None);
            }
        }
    } else if (op == "rename") {
        const std::size_t row = model.find(arg);
        if (row != ui::kNoRow) {
            model.click_row(row, Modifiers::None);
            begin_inline_rename();
        }
    } else if (op == "dragstart") {
        const std::size_t row = model.find(arg);
        if (row != ui::kNoRow) {
            drag_begin(workspace->active_tab().value, row);
            drag_debug_hold = drag.dragging();
        }
    } else if (op == "dragover") {
        // dragover:row:NAME | sidebar:LABEL | pane:N | trash
        const auto at = find_slot(raw.substr(std::string("dragover:").size()));
        if (at != slots.end() && drag.dragging()) {
            pointer.x = at->rect.center_x();
            pointer.y = at->rect.center_y();
            pointer.inside = true;
            drag.update(pointer.x, pointer.y, pointer.mods, slots, now_ms(),
                        ui::default_drop_probe());
        }
    } else if (op == "dragend") {
        drag_debug_hold = false;
        pointer.released = true;
    } else if (op == "dragcancel") {
        drag.cancel();
        drag_debug_hold = false;
    }
}

std::vector<ui::DropSlot>::const_iterator App::Impl::find_slot(std::string_view spec) const {
    const std::size_t colon = spec.find(':');
    const std::string kind(spec.substr(0, colon));
    const std::string value =
        colon == std::string_view::npos ? std::string() : std::string(spec.substr(colon + 1));
    for (auto it = slots.begin(); it != slots.end(); ++it) {
        if (kind == "row" && it->kind == ui::SlotKind::Row && it->label == value) {
            return it;
        }
        if (kind == "sidebar" && it->kind == ui::SlotKind::Sidebar && it->label == value) {
            return it;
        }
        if (kind == "trash" && it->kind == ui::SlotKind::Trash) {
            return it;
        }
        if (kind == "tab" && it->kind == ui::SlotKind::Tab && it->label == value) {
            return it;
        }
        if (kind == "crumb" && it->kind == ui::SlotKind::Crumb && it->label == value) {
            return it;
        }
    }
    if (kind == "pane") {
        const layout::PaneId pane =
            pane_at_index(static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10)));
        const layout::PaneNode* node = workspace->layout().find_pane(pane);
        if (node != nullptr && node->active_tab < node->tabs.size()) {
            for (auto it = slots.begin(); it != slots.end(); ++it) {
                if (it->kind == ui::SlotKind::Pane &&
                    it->tab == node->tabs[node->active_tab].id.value) {
                    return it;
                }
            }
        }
    }
    return slots.end();
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
        inspector_view.clear_caches();
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
    Theme resolved_base = theme;
    const ui::Palette target_pal = resolve_palette(appearance_settings.theme_id, resolved_base);
    const ui::Palette pal = fade < 1.0F ? ui::mix(previous_palette, target_pal, fade) : target_pal;
    const std::string_view lang = language() == Language::Turkish ? "tr" : "en";
    const float font_scale =
        static_cast<float>(appearance::clamp_font_scale(appearance_settings.font_scale_percent)) *
        0.01F;
    const ui::Painter painter(*renderer, *text, *fonts, scale, lang, font_scale,
                              appearance_settings.corner_radius_dp);
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
        .alternating_rows = appearance_settings.alternating_rows,
        .density = density,
        .spacing_scale_percent = appearance_settings.spacing_scale_percent,
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
        .list_keyboard =
            ui::keyboard_owner(view.popup_open(), view.address_open() || sidebar.renaming(),
                               sidebar_focus) == ui::KeyboardOwner::List &&
            !filter_focused(),
        .draw_sidebar = {},
        .draw_sidebar_overlay = {},
        .filter_focus = filter_focused() && focused ? workspace->active_tab() : layout::TabId{},
        .index_status = [this](const Path& dir) -> std::optional<search::IndexStatus> {
            // `dir` is a flattened volume root: only that volume's own index speaks for it.
            if (const std::shared_ptr<search::IIndex> i = indexes->index_for(dir);
                i && i->root() == dir) {
                return i->status();
            }
            return std::nullopt;
        },
        .draw_inspector = {},
        .thumbnails = nullptr,
        .file_ops = file_ops.get(),
        .slots = &slots,
        .file_drag_active = drag.dragging(),
        .is_trash_tab = {},
        .draw_trash = {},
        .tab_title = {},
        .ops_chip_width = {},
        .draw_ops_chip = {},
        .draw_ops_overlay = {},
        .ops_hit = {},
        .ops_modal = ops_view.modal(),
        .git_status = git_service.get(),
        .plugins = &plugin_manager,
    };
    slots.clear(); // the views register this frame's drop slots while drawing
    // Phase 4: the sidebar takes the left of the window; the panes get the rest.
    layout_sidebar(static_cast<float>(w), static_cast<float>(h));
    const bool sidebar_shown = sidebar_rect.w > 0.0F;
    frame.content = ui::RectF{sidebar_rect.w, 0.0F, static_cast<float>(w) - sidebar_rect.w,
                              static_cast<float>(h)};
    // Phase 6: the Inspector docks right of or below the pane tree.
    {
        layout::InspectorState st = workspace->inspector();
        st.visible = st.visible && inspector.visible();
        dock =
            ui::inspector::dock_inspector(st, frame.content, scale, static_cast<double>(w) / scale,
                                          static_cast<double>(h) / scale);
        frame.content = dock.list;
    }
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
        .slots = &slots,
    };
    if (sidebar_shown) {
        frame.draw_sidebar = [this, &sf] { sidebar.draw(sf); };
        frame.draw_sidebar_overlay = [this, &sf] { sidebar.draw_overlay(sf); };
    }
    // Phase 6: list thumbnails and the Inspector (drawn by the workspace view between the panes
    // and the overlays, so popups and toasts cover it).
    const bool masked_ptr = popup || view.dragging();
    if (list_thumbs) {
        list_thumbs->begin_frame(t_ms, animations);
        frame.thumbnails = list_thumbs.get();
    }
    thumbs.begin_frame();
    if (dock.inspector.w > 0.0F && textures) {
        frame.draw_inspector = [this, &painter, &pal, masked_ptr, &masked, t_ms, dt_ms, now] {
            draw_inspector(painter, pal, masked_ptr ? masked : pointer, t_ms, dt_ms, now);
        };
    }
    // Phase 7: the Trash view, the progress chip, the overlays and the drag badge.
    const bool kb_list = frame.list_keyboard;
    frame.is_trash_tab = [this](layout::TabId tab) { return trash_tabs.contains(tab.value); };
    frame.tab_title = [this](layout::TabId tab) -> std::optional<std::string> {
        if (trash_tabs.contains(tab.value)) {
            return std::string(tr("trash.title"));
        }
        return std::nullopt;
    };
    frame.draw_trash = [this, &painter, &pal, kb_list, t_ms, dt_ms,
                        now](layout::TabId tab, const ui::RectF& body, const ui::Palette& pane_pal,
                             const ui::PointerInput& ptr, bool active, bool marked) {
        ui::TrashFrame tf{painter,
                          pane_pal,
                          theme,
                          *trash_model,
                          ptr,
                          queue,
                          intents,
                          body,
                          tab.value,
                          focused && active && kb_list,
                          active,
                          marked,
                          animations,
                          density,
                          dt_ms,
                          now,
                          home.empty() ? nullptr : &home};
        (void)pal;
        (void)t_ms;
        trash_view.draw(tf);
    };
    ui::DragVisual visual;
    const bool pointer_in_window = pointer.x >= 0.0F && pointer.y >= 0.0F &&
                                   pointer.x <= static_cast<float>(w) &&
                                   pointer.y <= static_cast<float>(h);
    if (drag.dragging()) {
        visual.active = true;
        visual.eval = drag.eval();
        visual.count = drag.items().size();
        visual.first = drag_first;
        visual.x = pointer.x;
        visual.y = pointer.y;
        visual.pointer_inside = pointer_in_window;
        visual.outgoing_note =
            std::string(tr(outgoing_live ? "drag.external_ok" : "drag.use_clipboard"));
    } else if (incoming.active() && incoming.has_position()) {
        visual.active = true;
        visual.external = true;
        visual.eval = incoming_eval;
        visual.count = incoming.paths().size();
        visual.first = incoming_first;
        visual.x = incoming.x();
        visual.y = incoming.y();
    }
    window_w = static_cast<float>(w);
    window_h = static_cast<float>(h);
    const ui::PointerInput& ops_ptr = popup ? masked : pointer;
    const auto make_ops_frame = [&](float lift) {
        return ui::OpsFrame{
            .painter = painter,
            .palette = pal,
            .theme = theme,
            .ops = *file_ops,
            .pointer = ops_ptr,
            .queue = queue,
            .intents = intents,
            .width = static_cast<float>(w),
            .height = static_cast<float>(h),
            .content = frame.content,
            .density = density,
            .now_ms = t_ms,
            .dt_ms = dt_ms,
            .animations = animations,
            .now = now,
            .notice_lift_px = lift,
            .thumb =
                [this, &pal](const Path& path, const fs::DirEntry& e, const ui::RectF& box) {
                    return list_thumbs != nullptr &&
                           list_thumbs->draw_entry(*renderer, pal, path, e, box, scale, 1.0F);
                },
            .drag = &visual,
        };
    };
    frame.ops_chip_width = [this, &make_ops_frame] {
        return ops_view.chip_width(make_ops_frame(0.0F));
    };
    frame.draw_ops_chip = [this, &make_ops_frame](const ui::RectF& slot) {
        ops_view.draw_chip(make_ops_frame(0.0F), slot);
    };
    frame.draw_ops_overlay = [this, &make_ops_frame](float lift) {
        ops_view.draw_overlays(make_ops_frame(lift));
    };
    frame.ops_hit = [this](float x, float y) { return ops_view.hit(x, y); };
    view.update(frame);
    view.draw(frame);
    // Phase 8 overlays (drawn above workspace and ops overlays).
    const ui::RectF full_viewport{0.0F, 0.0F, static_cast<float>(w), static_cast<float>(h)};
    if (context_menu.is_open()) {
        if (const auto act = context_menu.draw(painter, pal, full_viewport, pointer)) {
            apply_menu_action(*act);
        }
    }
    if (palette_overlay.is_open()) {
        if (const auto act =
                palette_overlay.draw(painter, pal, full_viewport, pointer, palette_history)) {
            apply_palette_action(*act);
        }
    }
    if (content_search_overlay.open()) {
        if (const auto picked = content_search_overlay.draw(
                painter, pal, pointer, static_cast<float>(w), static_cast<float>(h));
            picked) {
            leave_trash_view();
            workspace->navigate_active(picked->parent_path(), picked->filename().native());
        }
    }
    if (openwith_overlay.is_open()) {
        if (const auto act = openwith_overlay.draw(painter, pal, full_viewport, pointer)) {
            launch_openwith_handler(act->handler, act->paths, act->primary_mime,
                                    act->set_as_default);
        }
    }
    if (keymap_settings.is_open()) {
        const ui::Palette before_pal = pal;
        const std::string before_theme = appearance_settings.theme_id;
        const ui::KeymapEditorEvent ev =
            keymap_settings.draw(painter, pal, full_viewport, pointer, keymap_file, keymap,
                                 default_keymap, registry, appearance_settings, custom_themes);
        if (ev.keymap_dirty) {
            keymap_dirty = true;
            rebuild_keymap_from_file();
        }
        if (ev.appearance_dirty) {
            appearance_dirty = true;
            const bool theme_changed = appearance_settings.theme_id != before_theme;
            apply_appearance_settings();
            if (theme_changed) {
                previous_palette = before_pal;
                fade_start_ms = animations ? now_ms() : -1.0;
            }
        }
        if (ev.enqueue_cmd) {
            queue.enqueue(*ev.enqueue_cmd);
        }
    }
    if (batch_rename_overlay.is_open()) {
        const ui::BatchRenameEvent ev =
            batch_rename_overlay.draw(painter, pal, full_viewport, pointer);
        if (ev.request_paste_clipboard) {
            if (char* t = SDL_GetClipboardText(); t != nullptr) {
                batch_rename_overlay.paste_text(t);
                SDL_free(t);
            }
        }
        if (ev.apply_request) {
            file_ops->submit_batch_rename(*ev.apply_request);
        }
    }
    if (reset_keymap_confirm) {
        renderer->fill_rect(full_viewport, pal.shadow.with_alpha(0.5F));
        const float cw = std::min(painter.px(420.0F), full_viewport.w - painter.px(32.0F));
        const float ch = painter.px(148.0F);
        const ui::RectF card{std::floor((full_viewport.w - cw) * 0.5F),
                             std::floor((full_viewport.h - ch) * 0.5F), cw, ch};
        const float rad = painter.px(ui::radius::lg);
        renderer->drop_shadow(card, rad, painter.px(16.0F), painter.px(5.0F),
                              pal.shadow.with_alpha(0.35F));
        renderer->fill_rounded_rect(card, rad, pal.surface_raised);
        renderer->stroke_rounded_rect(card, rad, painter.px(1.0F), pal.danger_border);
        painter.draw(painter.shape(ui::type::popup_title, tr("keymap.reset_confirm_title")),
                     card.x + painter.px(16.0F), card.y + painter.px(30.0F), pal.danger);
        painter.draw(painter.shape(ui::type::caption, tr("keymap.reset_confirm_body")),
                     card.x + painter.px(16.0F), card.y + painter.px(58.0F), pal.text_secondary);
        const float bw = painter.px(110.0F);
        const float bh = painter.px(30.0F);
        const float by = card.y + card.h - bh - painter.px(16.0F);
        const ui::RectF reset_btn{card.x + card.w - bw - painter.px(16.0F), by, bw, bh};
        const ui::RectF cancel_btn{reset_btn.x - bw - painter.px(10.0F), by, bw, bh};
        renderer->fill_rounded_rect(cancel_btn, painter.px(6.0F), pal.surface);
        renderer->stroke_rounded_rect(cancel_btn, painter.px(6.0F),
                                      painter.px(reset_confirm_focus == 0 ? 2.0F : 1.0F),
                                      reset_confirm_focus == 0 ? pal.accent : pal.border);
        const ui::ShapedText ct = painter.shape(ui::type::button, tr("action.cancel"));
        painter.draw(ct, cancel_btn.center_x() - ct.width() * 0.5F, cancel_btn.y + bh * 0.67F,
                     pal.text_primary);
        renderer->fill_rounded_rect(reset_btn, painter.px(6.0F), pal.danger_surface);
        renderer->stroke_rounded_rect(reset_btn, painter.px(6.0F),
                                      painter.px(reset_confirm_focus == 1 ? 2.0F : 1.0F),
                                      pal.danger);
        const ui::ShapedText rt = painter.shape(ui::type::button, tr("keymap.reset_cmd"));
        painter.draw(rt, reset_btn.center_x() - rt.width() * 0.5F, reset_btn.y + bh * 0.67F,
                     pal.danger);
        if (pointer.pressed) {
            if (contains(cancel_btn, pointer.x, pointer.y) ||
                !contains(card, pointer.x, pointer.y)) {
                reset_keymap_confirm = false;
            } else if (contains(reset_btn, pointer.x, pointer.y)) {
                if (reset_all(keymap_file)) {
                    keymap_dirty = true;
                    rebuild_keymap_from_file();
                }
                reset_keymap_confirm = false;
            }
        }
    }
    if (!seq_status_text.empty()) {
        const ui::ShapedText st = painter.shape(ui::type::keycap, seq_status_text);
        const float pw = st.width() + painter.px(16.0F);
        const float ph = painter.px(24.0F);
        const ui::RectF pill{full_viewport.w - pw - painter.px(12.0F),
                             full_viewport.h - ph - painter.px(32.0F), pw, ph};
        renderer->fill_rounded_rect(pill, painter.px(6.0F), pal.surface_raised);
        renderer->stroke_rounded_rect(pill, painter.px(6.0F), painter.px(1.0F), pal.accent);
        painter.draw(st, pill.x + painter.px(8.0F), pill.y + ph * 0.70F, pal.text_primary);
    }
    thumbs.end_frame();
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

void App::Impl::bench_flatten_step(double frame_ms) {
    // Phase 5 acceptance (tests/ui/flatten_smoke.sh): flatten a large generated tree, keep
    // rendering while it streams, cancel, check the walk thread exits, report, quit. Frame times
    // cover the whole walk: applying batches, organizing results and drawing.
    if (!workspace->settled()) {
        return;
    }
    ui::BrowserModel& m = workspace->active_model();
    const double t = now_ms();
    switch (flat_phase) {
    case 0:
        queue.enqueue(CommandId::ToggleFlatten);
        flat_t0 = t;
        flat_phase = 1;
        return;
    case 1: {
        flat_frames.push_back(frame_ms);
        const ui::FlattenProgress prog = m.flatten_progress();
        flat_found = prog.found;
        if (!flat_filtered && prog.found >= 20000) {
            flat_filtered = true; // a keystroke mid-walk re-matches everything collected so far
            m.set_filter_text("re");
        }
        // Cancel mid-walk: after a good share of the tree streamed in, or a time cap.
        if (prog.found >= 60000 || t - flat_t0 > 8000.0 || (!prog.walking && prog.found > 0)) {
            flat_cancel_ms = t;
            flat_cancelled = prog.walking;
            queue.enqueue(CommandId::CancelSearch);
            flat_phase = 2;
        }
        return;
    }
    case 2:
        flat_frames.push_back(frame_ms);
        if (ui::FlattenWalk::live_threads() == 0) {
            flat_exit_ms = t - flat_cancel_ms;
            flat_phase = 3;
            flat_t0 = t;
        } else if (t - flat_cancel_ms > 5000.0) {
            flat_exit_ms = -1.0; // did not exit in time: reported as a failure
            flat_phase = 3;
            flat_t0 = t;
        }
        return;
    case 3:
        // Let the last rows land, then leave the view (freeing it is part of the measurement).
        flat_frames.push_back(frame_ms);
        if (!m.searching() || t - flat_t0 > 5000.0) {
            std::printf("bench-flatten: found=%llu rows=%zu cancelled_midwalk=%d\n",
                        static_cast<unsigned long long>(m.flatten_progress().found), m.row_count(),
                        flat_cancelled ? 1 : 0);
            queue.enqueue(CommandId::ToggleFlatten);
            flat_phase = 4;
            flat_t0 = t;
        }
        return;
    case 4:
        flat_frames.push_back(frame_ms);
        if (t - flat_t0 < 250.0) {
            return;
        }
        std::printf("bench-flatten: frames=%zu frame_avg_ms=%.2f frame_p95_ms=%.2f "
                    "frame_max_ms=%.2f\n",
                    flat_frames.size(), mean(flat_frames), percentile(flat_frames, 0.95),
                    percentile(flat_frames, 1.0));
        std::printf("bench-flatten: cancel_to_exit_ms=%.1f threads_left=%d\n", flat_exit_ms,
                    ui::FlattenWalk::live_threads());
        (void)std::fflush(stdout);
        running = false;
        return;
    default: return;
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

// ---------------------------------------------------------------------------------------------
// Phase 6: Inspector and list thumbnails
// ---------------------------------------------------------------------------------------------

void App::Impl::init_previews(const AppConfig& config) {
    ui::ThumbnailRootInput ti;
    ti.thumbnail_dir = config.thumbnail_dir;
    ti.cache_dir = config.cache_dir;
    ti.headless = config.max_frames > 0;
    ti.enable_thumbnails = config.enable_thumbnails;
    if (config.enable_thumbnails && config.thumbnail_dir.empty() && config.cache_dir.empty() &&
        config.max_frames <= 0) {
        ti.platform_root = ui::platform_thumbnail_root();
    }
    ui::PreviewBackendOptions bo;
    bo.thumbnail_root = ui::resolve_thumbnail_root(ti);
    bo.external_thumbnailers = config.enable_thumbnails && config.enable_external_thumbnailers;
    bo.cache_bytes = config.preview_cache_bytes;
    const Uint32 wake_type = wake_event;
    bo.wake = [wake_type] {
        if (wake_type != 0) {
            SDL_Event e{};
            e.type = wake_type;
            SDL_PushEvent(&e); // thread-safe
        }
    };
    if (config.max_frames > 0 && config.thumbnail_dir.empty() && config.cache_dir.empty()) {
        SDL_Log("Navigator: --frames without --thumbnail-dir/--cache-dir: thumbnail cache off");
    }
    previews = ui::create_preview_backend(bo);
    textures = std::make_unique<ui::inspector::TextureCache>(
        *renderer, std::max<std::size_t>(static_cast<std::size_t>(config.preview_cache_bytes),
                                         ui::inspector::kDefaultTextureBudgetBytes));
    thumbs.set_service(previews.get());
    thumbs.set_enabled(config.enable_thumbnails);
    inspector.set_service(previews.get());
    list_thumbs = std::make_unique<ui::inspector::ListThumbnails>(
        thumbs, *textures, [this](const Path& path) { return location_for(path); });
    inspector.set_visible(workspace->inspector().visible, now_ms());
}

preview::SourceLocation App::Impl::location_for(const Path& path) {
    const std::vector<VolumeInfo>& vols = places->volumes();
    if (vols.size() != locations_volumes) {
        locations.clear();
        locations_volumes = vols.size();
    }
    const std::string dir = path.parent_path().native();
    if (const auto it = locations.find(dir); it != locations.end()) {
        return it->second;
    }
    preview::SourceLocation loc;
    if (const std::optional<std::size_t> v =
            places::containing_volume(path, Span<const VolumeInfo>(vols.data(), vols.size()));
        v) {
        loc.network = vols[*v].kind == VolumeKind::Network;
        loc.removable = vols[*v].removable;
    }
    if (locations.size() > 4096) {
        locations.clear();
    }
    locations.emplace(dir, loc);
    return loc;
}

ui::inspector::Item App::Impl::item_for(const ui::BrowserModel& m, std::size_t row) {
    ui::inspector::Item it;
    it.path = m.row_path(row);
    it.entry = m.row_entry(row);
    it.location = location_for(it.path);
    it.folder.show_hidden = m.show_hidden();
    it.folder.sort = m.sort();
    return it;
}

ui::inspector::Subject App::Impl::inspector_subject() const {
    ui::inspector::Subject s;
    const ui::BrowserModel& m = workspace->active_model();
    if (!m.loaded() || trash_active()) {
        return s;
    }
    auto* self = const_cast<App::Impl*>(this); // location cache only
    const std::size_t cur = m.cursor();
    if (cur < m.row_count()) {
        s.cursor = self->item_for(m, cur);
        for (std::size_t d = 1; d <= ui::inspector::kPrefetchNeighbours; ++d) {
            if (cur + d < m.row_count()) {
                s.neighbours.push_back(self->item_for(m, cur + d));
            }
            if (cur >= d) {
                s.neighbours.push_back(self->item_for(m, cur - d));
            }
        }
    }
    s.selected_count = m.selected_count();
    s.selected_bytes = m.selected_bytes();
    return s;
}

void App::Impl::previews_frame_start(double t_ms) {
    if (previews) {
        completions.clear();
        if (previews->poll(completions) > 0) {
            for (const preview::Completion& c : completions) {
                if (!inspector.on_completion(c)) {
                    (void)thumbs.on_completion(c);
                }
            }
            dirty = true;
        }
    }
    if (textures && textures->upload(t_ms)) {
        dirty = true;
    }
    if (inspector.visible()) {
        const std::uint64_t before = inspector.serial();
        inspector.follow(inspector_subject(), t_ms);
        // A locked item that left its folder (deleted, renamed away) unlocks with a notice.
        if (inspector.locked() && inspector.item() != nullptr) {
            const ui::BrowserModel& m = workspace->active_model();
            if (m.content_serial() != locked_seen_content) {
                locked_seen_content = m.content_serial();
                const ui::inspector::Item& it = *inspector.item();
                if (m.loaded() && m.current_dir() == it.path.parent_path() &&
                    m.find(it.path.filename().native()) == ui::kNoRow &&
                    inspector.unlock_if_gone(false)) {
                    toast(std::string(tr("inspector.unlocked_gone")));
                }
            }
        }
        inspector.tick(t_ms, thumbs.epoch());
        if (inspector.serial() != before) {
            dirty = true;
        }
    }
}

bool App::Impl::previews_busy() const noexcept {
    return (inspector.visible() && (inspector.busy() || inspector_view.animating())) ||
           (textures && textures->uploading()) || (list_thumbs && list_thumbs->fading()) ||
           thumbs.pending_count() > 0;
}

void App::Impl::set_inspector_visible(bool visible) {
    layout::InspectorState st = workspace->inspector();
    st.visible = visible;
    workspace->set_inspector(st);
    inspector.set_visible(visible, now_ms());
    if (!visible) {
        inspector_focus = false;
    }
}

bool App::Impl::route_inspector(const Command& command) {
    switch (command.id) {
    case CommandId::CursorUp:
    case CommandId::CursorDown:
    case CommandId::CursorPageUp:
    case CommandId::CursorPageDown:
    case CommandId::CursorHome:
    case CommandId::CursorEnd: inspector_view.cursor(command.id); return true;
    case CommandId::Activate: queue.enqueue(CommandId::InspectorPlay); return true;
    case CommandId::ClearSelection: inspector_focus = false; return true;
    case CommandId::Inspect: set_inspector_visible(false); return true;
    default: return false;
    }
}

void App::Impl::inspector_command(const Command& command) {
    layout::InspectorState st = workspace->inspector();
    switch (command.id) {
    case CommandId::Inspect:
        if (!st.visible) {
            focus_sidebar(false);
        }
        set_inspector_visible(!st.visible);
        return;
    case CommandId::ToggleInspectorLock: inspector.toggle_lock(); return;
    case CommandId::InspectorZoomFit: inspector_view.zoom_fit(); return;
    case CommandId::InspectorZoom1to1: inspector_view.zoom_1to1(); return;
    case CommandId::InspectorPlay: {
        const ui::inspector::Item* it = inspector.item();
        if (it == nullptr || fs::is_directory_like(it->entry) || !st.visible) {
            return;
        }
        if (!launcher) {
            toast(std::string(tr("inspector.play_failed")), true);
            return;
        }
        if (Result<void> r = platform::open_with_default(it->path, *launcher); !r) {
            toast(std::string(tr("inspector.play_failed")), true);
        }
        return;
    }
    case CommandId::CycleInspectorPlacement:
        st.placement = layout::next_placement(st.placement);
        workspace->set_inspector(st);
        return;
    case CommandId::ToggleInspectorGrid:
        st.show_grid = !st.show_grid;
        workspace->set_inspector(st);
        return;
    case CommandId::ToggleInspectorMinimap:
        st.show_minimap = !st.show_minimap;
        workspace->set_inspector(st);
        return;
    default: return;
    }
}

void App::Impl::draw_inspector(const ui::Painter& painter, const ui::Palette& pal,
                               const ui::PointerInput& ptr, double t_ms, double dt_ms,
                               Timestamp now) {
    if (dock.inspector.w <= 0.0F || dock.inspector.h <= 0.0F) {
        return;
    }
    // The item's list thumbnail: a low-resolution placeholder while the image decodes.
    const ui::inspector::TextureCache::Texture* placeholder = nullptr;
    if (const ui::inspector::Item* it = inspector.item(); it != nullptr && textures) {
        const float box = std::round(painter.px(ui::metrics::icon_size + 2.0F));
        const auto side = static_cast<std::uint32_t>(std::max(1.0F, std::ceil(box)));
        preview::PreviewRequest r =
            ui::make_request(it->path, it->entry, preview::PreviewPurpose::Thumbnail,
                             preview::PixelSize{side, side});
        r.location = it->location;
        placeholder = textures->get(ui::preview_key(r));
    }
    const layout::InspectorState& st = workspace->inspector();
    const ui::inspector::InspectorFrame f{
        .painter = painter,
        .palette = pal,
        .theme = theme,
        .pointer = ptr,
        .queue = queue,
        .controller = inspector,
        .state = st,
        .textures = *textures,
        .dock = dock.inspector,
        .content = ui::RectF{dock.list.x, dock.list.y,
                             dock.list.w + (dock.placement == layout::InspectorPlacement::Right
                                                ? dock.inspector.w
                                                : 0.0F),
                             dock.list.h + (dock.placement == layout::InspectorPlacement::Bottom
                                                ? dock.inspector.h
                                                : 0.0F)},
        .splitter = dock.splitter,
        .placement = dock.placement,
        .keyboard = inspector_focus,
        .window_focused = focused,
        .animations = animations,
        .density = density,
        .now_ms = t_ms,
        .dt_ms = dt_ms,
        .now = now,
        .placeholder = placeholder,
    };
    inspector_view.draw(f, inspector_out);
    inspector.set_view_target(inspector_view.fit_target(), inspector_view.wants_full_resolution(),
                              t_ms);
}

void App::Impl::apply_inspector_output() {
    ui::inspector::InspectorOutput out = std::move(inspector_out);
    inspector_out = {};
    if (out.resize) {
        layout::InspectorState st = workspace->inspector();
        if (dock.placement == layout::InspectorPlacement::Bottom) {
            st.height = *out.resize;
        } else {
            st.width = *out.resize;
        }
        workspace->set_inspector(st);
    }
    if (out.pressed_inside && !view.popup_open()) {
        inspector_focus = true;
        focus_sidebar(false);
        filter_tab = layout::TabId{};
        view.close_address_bar();
    } else if (pointer.pressed && inspector_focus &&
               !contains(inspector_view.card(), pointer.x, pointer.y)) {
        inspector_focus = false;
    }
    if (out.copy) {
        set_clipboard(*out.copy);
        toast(std::string(tr("inspector.copied")));
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
        if (s.seq_matcher.expire(SequenceMatcher::Clock::now())) {
            s.seq_status_text.clear();
            s.dirty = true;
        }
        if (s.workspace->frame_start()) {
            s.dirty = true;
        }
        if (s.places->frame_start()) {
            s.dirty = true;
        }
        s.sync_index_volumes();
        s.previews_frame_start(now_ms());
        // Phase 7: finished operations, conflicts, the Trash listing, reconciliation.
        if (s.file_ops->frame_start(now_ms())) {
            s.dirty = true;
        }
        if (s.trash_model->frame_start()) {
            s.dirty = true;
        }
        s.apply_reconcile(now_ms());
        s.process_pending(now_ms());
        if (s.outgoing && s.outgoing->active()) {
            (void)s.outgoing->poll(now_ms());
            s.dirty = true;
        }
        s.sync_protected_paths();
        if (s.file_ops->quit_state() == ui::QuitState::Quit) {
            s.running = false;
        }
        // A background scan reports often: repaint only while a status bar shows the index.
        if (s.indexes->frame_start() && s.shows_index_status()) {
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
        if (const layout::TabId active_tab = s.workspace->active_tab(); active_tab.valid()) {
            if (ui::BrowserModel* m = s.workspace->model(active_tab); m != nullptr) {
                s.sync_tab_view_settings(*m, active_tab.value);
            }
        }
        if (s.view.poll_paths(now_ms(), s.outcomes)) {
            s.dirty = true;
        }
        // Phase 10: poll single-instance IPC, Git status worker, and Content Search overlay.
        if (s.ipc_server) {
            for (const platform::IpcMessage& msg : s.ipc_server->poll()) {
                SDL_RaiseWindow(s.window.get());
                s.dirty = true;
                if (msg.action == platform::IpcAction::SelectPath && !msg.path.empty()) {
                    s.leave_trash_view();
                    (void)s.workspace->open_tab(msg.path.parent_path(), false);
                    s.workspace->navigate_active(msg.path.parent_path(),
                                                 msg.path.filename().native());
                } else if ((msg.action == platform::IpcAction::NewTab ||
                            msg.action == platform::IpcAction::OpenPath) &&
                           !msg.path.empty()) {
                    s.leave_trash_view();
                    (void)s.workspace->open_tab(msg.path, false);
                }
            }
        }
        if (s.git_service) {
            const Path cur_dir = s.workspace->active_model().current_dir();
            if (!cur_dir.empty() && cur_dir != s.last_git_dir) {
                s.last_git_dir = cur_dir;
                s.git_service->request_directory(cur_dir);
            }
            if (s.git_service->poll()) {
                s.dirty = true;
            }
        }
        if (s.content_search_overlay.poll()) {
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
        if (!s.first_frame_done) {
            // Phase 5: indexes start only after the first frame is on screen.
            s.first_frame_done = true;
            s.indexes->start();
        }

        // 4. Frame end: pointer intents, then queued commands (FIFO; commands they enqueue run
        //    next frame).
        // A press outside the sidebar moves keyboard focus back to the panes.
        if (s.pointer.pressed && s.sidebar_focus &&
            !contains(s.sidebar_rect, s.pointer.x, s.pointer.y) && !s.view.popup_open()) {
            s.focus_sidebar(false);
        }
        // Phase 5: a press outside the filter field returns the keyboard to the list.
        if (s.pointer.pressed && s.filter_focused()) {
            const ui::BrowserView* fv = s.view.find_view(s.workspace->active_tab());
            if (fv == nullptr || !contains(fv->filter_rect(), s.pointer.x, s.pointer.y)) {
                s.filter_tab = layout::TabId{};
            }
        }
        s.apply_intents();
        s.drag_frame_end(now_ms());
        s.apply_outcomes();
        s.apply_inspector_output();
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
        // Navigation hands the keyboard back to the list (the filter text was cleared too).
        if (s.filter_tab.valid()) {
            const ui::BrowserModel* fm = s.workspace->model(s.filter_tab);
            if (fm == nullptr || s.filter_tab != s.workspace->active_tab() ||
                fm->current_dir() != s.filter_dir) {
                s.filter_tab = layout::TabId{};
            }
        }
        (void)s.workspace->sync();
        s.persist_tick(now_ms());
        if (s.textures) {
            s.textures->end_frame();
        }
        s.cursors.show(s.sidebar.resize_cursor() ? ui::PointerCursor::ResizeHorizontal
                       : s.inspector_view.resize_hot()
                           ? (s.dock.placement == layout::InspectorPlacement::Bottom
                                  ? ui::PointerCursor::ResizeVertical
                                  : ui::PointerCursor::ResizeHorizontal)
                           : s.view.cursor());
        // Phase 5: text input stays on while the window has focus, so typing in a list starts
        // its filter (type-to-filter); chords still arrive as key events.
        const bool want_text =
            s.focused || s.view.wants_text_input() || s.sidebar.renaming() ||
            s.palette_overlay.wants_text_input() || s.context_menu.wants_text_input() ||
            s.openwith_overlay.wants_text_input() || s.keymap_settings.wants_text_input() ||
            s.content_search_overlay.open();
        if (want_text != s.text_input_on) {
            s.text_input_on = want_text;
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
        if (s.bench_flatten) {
            // Whole loop turn: applying batches at frame start counts, not only drawing.
            s.bench_flatten_step(end_ms - last_ms);
        }
        if (s.inspector.visible() && s.inspector.item() != nullptr) {
            s.inspector_max_ms = std::max(s.inspector_max_ms, end_ms - t_ms);
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
