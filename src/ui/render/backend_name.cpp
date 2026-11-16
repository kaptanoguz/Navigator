// src/ui/render/backend_name.cpp — see backend_name.hpp.
#include "render/backend_name.hpp"

#include <array>

#include <navigator/i18n.hpp>

namespace navigator::ui {
namespace {

struct Name {
    std::string_view id;
    std::string_view display;
};

// SDL_Renderer drivers and SDL_GPU device drivers share these ids.
constexpr std::array kNames{
    Name{"vulkan", "Vulkan"},
    Name{"opengl", "OpenGL"},
    Name{"opengles2", "OpenGL ES 2"},
    Name{"opengles", "OpenGL ES"},
    Name{"metal", "Metal"},
    Name{"direct3d12", "Direct3D 12"},
    Name{"direct3d11", "Direct3D 11"},
    Name{"direct3d", "Direct3D 9"},
};

std::string_view pretty(std::string_view id) noexcept {
    for (const Name& n : kNames) {
        if (n.id == id) {
            return n.display;
        }
    }
    return id;
}

} // namespace

std::string backend_display_name(std::string_view renderer, std::string_view gpu_driver) {
    if (renderer.empty()) {
        return "Unknown";
    }
    if (renderer == "software") {
        return std::string(tr("fps.renderer.software"));
    }
    if (renderer == "gpu") {
        if (gpu_driver.empty()) {
            return "SDL_GPU";
        }
        return std::string(pretty(gpu_driver)) + " (SDL_GPU)";
    }
    return std::string(pretty(renderer));
}

} // namespace navigator::ui
