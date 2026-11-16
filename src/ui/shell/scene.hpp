// src/ui/shell/scene.hpp
//
// The Phase 0 scene: centered wordmark + tagline, keyboard hint chips, optional FPS overlay.
// Reads a SceneModel (plain state snapshot) and draws through a Painter. Never mutates state.
// Thread-safety: UI thread only.
#pragma once

#include <string_view>

#include <SDL3/SDL.h>

#include "shell/painter.hpp"
#include "theme/theme.hpp"

namespace navigator::ui {

struct SceneModel {
    Palette palette;
    float width = 0.0F; // px
    float height = 0.0F;
    SDL_Texture* background_from = nullptr; // previous theme during a crossfade (or nullptr)
    SDL_Texture* background_to = nullptr;
    float background_t = 1.0F;
    bool show_fps = false;
    double fps = 0.0;
    double frame_ms = 0.0;
    std::string_view render_driver;
};

void draw_scene(const Painter& painter, const SceneModel& model);

} // namespace navigator::ui
