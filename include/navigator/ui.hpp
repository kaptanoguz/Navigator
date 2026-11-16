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
    bool save_session = true;
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
