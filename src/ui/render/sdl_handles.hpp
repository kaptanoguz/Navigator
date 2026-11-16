// src/ui/render/sdl_handles.hpp — RAII owners for SDL handles. UI thread only.
#pragma once

#include <memory>

#include <SDL3/SDL.h>

namespace navigator::ui {

struct SdlWindowDeleter {
    void operator()(SDL_Window* w) const noexcept { SDL_DestroyWindow(w); }
};
struct SdlRendererDeleter {
    void operator()(SDL_Renderer* r) const noexcept { SDL_DestroyRenderer(r); }
};
struct SdlTextureDeleter {
    void operator()(SDL_Texture* t) const noexcept { SDL_DestroyTexture(t); }
};
struct SdlSurfaceDeleter {
    void operator()(SDL_Surface* s) const noexcept { SDL_DestroySurface(s); }
};

using WindowPtr = std::unique_ptr<SDL_Window, SdlWindowDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, SdlRendererDeleter>;
using TexturePtr = std::unique_ptr<SDL_Texture, SdlTextureDeleter>;
using SurfacePtr = std::unique_ptr<SDL_Surface, SdlSurfaceDeleter>;

// SDL_Init/SDL_Quit pairing for the video subsystem.
class SdlVideo {
public:
    SdlVideo() = default;
    SdlVideo(const SdlVideo&) = delete;
    SdlVideo& operator=(const SdlVideo&) = delete;
    SdlVideo(SdlVideo&&) = delete;
    SdlVideo& operator=(SdlVideo&&) = delete;
    ~SdlVideo() {
        if (initialized_) {
            SDL_Quit();
        }
    }
    [[nodiscard]] bool init() {
        initialized_ = SDL_Init(SDL_INIT_VIDEO);
        return initialized_;
    }

private:
    bool initialized_ = false;
};

} // namespace navigator::ui
