// src/ui/render/backend_name.hpp
//
// Human-readable name of the active render backend for the FPS overlay, e.g. "Vulkan (SDL_GPU)",
// "OpenGL", or the translated "Software". Pure function (no SDL) so tests can cover the mapping;
// the caller passes SDL_GetRendererName() and, for the "gpu" renderer, SDL_GetGPUDeviceDriver().
// Thread-safety: pure; the "Software" label is looked up with navigator::tr (see i18n.hpp).
#pragma once

#include <string>
#include <string_view>

namespace navigator::ui {

// `renderer`: SDL render driver name ("gpu", "vulkan", "opengl", "opengles2", "software", ...).
// `gpu_driver`: SDL_GPU device driver ("vulkan", "metal", "direct3d12", ...) or empty.
// Unknown names are passed through unchanged; an empty renderer gives "Unknown".
[[nodiscard]] std::string backend_display_name(std::string_view renderer,
                                               std::string_view gpu_driver);

} // namespace navigator::ui
