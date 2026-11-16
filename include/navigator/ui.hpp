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
    bool show_fps = true;      // initial FPS overlay state (ToggleFpsOverlay flips it)
    Theme theme = Theme::Dark; // initial theme (ToggleTheme flips it)
    int max_frames = 0;        // 0: run until Quit; >0: exit after N frames (headless smoke tests)
};

struct FrameStats {
    double fps = 0.0;              // smoothed frames per second
    double frame_ms = 0.0;         // duration of the last frame, milliseconds
    std::uint64_t frame_index = 0; // frames completed since run() started
};

// Later phases (forward declarations only; not part of the Phase 0 contract).
class Pane;
class Tab;
class Layout;

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
