// navigator/ui.hpp
//
// Purpose: renderer-agnostic contract for the application shell (window, frame loop, theme).
//          No SDL, GPU, font or ImGui types appear here; they live behind App's pimpl.
// Owner:   contracts (header); implementation in src/ui and src/app (role: ui).
// Thread-safety: App is created, run and destroyed on the main (UI) thread. No member may be
//                called from another thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <navigator/commands.hpp>
#include <navigator/types.hpp>

namespace navigator {

enum class Theme {
    Dark,
    Light,
};

struct AppConfig {
    std::string_view title = "Navigator"; // copied by App::create
    int width = 1280;                     // initial logical window size
    int height = 800;
    bool show_fps = false;     // initial FPS overlay state (ToggleFpsOverlay flips it)
    Theme theme = Theme::Dark; // initial theme (ToggleTheme flips it)
    int max_frames = 0;        // 0: run until Quit; >0: exit after N frames (headless smoke tests)
    // false: no UI animations (theme crossfade, smooth scroll); `--no-animations`. The environment
    // variable NAVIGATOR_NO_ANIMATIONS=1 also forces false.
    bool animations = true;

    // Phase 2. Directory the browser opens first; copied by App::create. Raw path bytes (need not
    // be UTF-8). Empty: platform::user_dir(UserDir::Home), or "/" if that fails. A relative path
    // resolves against the process working directory. A path that cannot be listed (missing,
    // permission denied, not a directory) does not fail App::create: the browser opens with an
    // empty list and an error notice, and NavigateUp / Refresh still work from there.
    std::string_view start_path;

    // Phase 3: session and layouts (docs/architecture.md "Phase 3").
    // Navigator config root holding session.json and layouts/; copied by App::create. Empty:
    // platform::user_dir(UserDir::Config) / layout::kConfigDirName (~/.config/navigator; honours
    // $XDG_CONFIG_HOME). Tests point it at a temporary directory.
    std::string_view config_dir;
    // Load <config_dir>/session.json at startup (tabs, panes, closed tabs, window geometry).
    // A missing or unreadable session is not an error: one tab on the start folder. With a
    // non-empty start_path the session is still restored and start_path opens as a new active
    // tab in the restored active pane (so `navigator DIR` never discards the saved session).
    bool restore_session = true;
    // Also apply the saved window geometry (size, maximized, X11 position). The command line
    // sets this false when the user passes an explicit size; width/height are used then.
    bool restore_window_geometry = true;
    // Write the session (debounced after changes, and on quit). false: never write it
    // (`--no-session` also clears restore_session; headless smoke tests use it).
    // Phase 4: these two flags also govern <config_dir>/bookmarks.json and recents.json
    // (places.hpp): restore_session = false starts with empty bookmarks and recents,
    // save_session = false never writes them.
    bool save_session = true;

    // Phase 5: name index (index.hpp, docs/architecture.md "Phase 5").
    // Navigator cache root holding index/<id>.idx; copied by App::create. Empty:
    // platform::user_dir(UserDir::Cache) / "navigator" (~/.cache/navigator; honours
    // $XDG_CACHE_HOME). Tests point it at a temporary directory.
    std::string_view cache_dir;
    // Build, load and watch name indexes of the default volumes (search::should_index_by_default)
    // in the background. false (`--no-index`; headless smoke tests): no index thread, no index
    // file is read or written, and the flattened view always walks the disk.
    bool enable_index = true;

    // Phase 6: previews and thumbnails (preview.hpp, thumbnail.hpp, preview_service.hpp;
    // docs/architecture.md "Phase 6").
    // Freedesktop thumbnail cache root; copied by App::create. Empty: <cache_dir>/thumbnails
    // when cache_dir is set (tests stay inside their temporary cache dir and never share),
    // else platform::thumbnail_cache_dir() (~/.cache/thumbnails, shared with other programs).
    // Headless rule (as for the session and the index): with max_frames > 0 and neither
    // thumbnail_dir nor cache_dir set, the disk cache is disabled: nothing is read from or
    // written to ~/.cache/thumbnails and no external thumbnailer runs.
    std::string_view thumbnail_dir;
    // Memory budget of the preview / thumbnail result cache in bytes (features.md "cache with
    // configurable size"; `--preview-cache-mb N`). Raised to preview::kMinCacheBudgetBytes when
    // lower. GPU textures are budgeted separately by the ui.
    std::uint64_t preview_cache_bytes = std::uint64_t{256} * 1024U * 1024U;
    // false (`--no-thumbnails`): no list thumbnails, no disk-cache reads or writes, no external
    // thumbnailers. The Inspector still decodes the inspected item in memory.
    bool enable_thumbnails = true;
    // false (`--no-external-thumbnailers`; tests): never run /usr/share/thumbnailers entries;
    // built-in decoders and existing cached thumbnails still work.
    bool enable_external_thumbnailers = true;
    // The thumbnail fade-in follows `animations` (features.md: no fade when animations are off).

    // Phase 7: file operations (docs/architecture.md "Phase 7"). Home trash root for the app's
    // ITrash (`--trash-dir DIR`); copied by App::create. Non-empty: the trash lives in DIR and
    // only there (per-volume trashes are off, so a fixture run never touches another volume).
    // Empty: the real XDG trash ($XDG_DATA_HOME/Trash and the per-volume trashes) in a normal
    // run. Headless rule (as for the session, the index and the thumbnails): with max_frames > 0
    // and no trash_dir, NO trash exists: trashing, restoring and emptying fail with Unsupported,
    // so a smoke test can never move a real file into the owner's trash.
    std::string_view trash_dir;
};

struct FrameStats {
    double fps = 0.0;              // smoothed frames per second
    double frame_ms = 0.0;         // duration of the last frame, milliseconds
    std::uint64_t frame_index = 0; // frames completed since run() started
};

class App {
public:
    // Creates the window and renderer. Errors: PlatformError (window/renderer creation failed),
    // InvalidArgument (non-positive size or negative max_frames).
    [[nodiscard]] static Result<std::unique_ptr<App>> create(const AppConfig& config);

    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    // Runs the frame loop: input -> build UI -> render -> drain commands(). Returns the process
    // exit code (0 on Quit, window close, or reaching max_frames).
    int run();

    // Queue drained at the end of every frame. Enqueue from widgets or from outside before run().
    [[nodiscard]] CommandQueue& commands() noexcept;

    [[nodiscard]] FrameStats frame_stats() const noexcept;
    [[nodiscard]] Theme theme() const noexcept;

private:
    struct Impl;
    explicit App(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace navigator
