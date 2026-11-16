// src/ui/shell/background.hpp
//
// Window backdrop: a soft radial glow behind the content over the base background color,
// computed in linear light and dithered (TPDF, +-1 LSB) so the very low-contrast gradient never
// bands. Generated on the CPU at half resolution and bilinearly upscaled; cached per theme and
// rebuilt only when the window size changes.
// Thread-safety: UI thread only.
#pragma once

#include <cstdint>
#include <vector>

#include <navigator/types.hpp>

#include "render/sdl_handles.hpp"
#include "theme/theme.hpp"

namespace navigator::ui {

// Pixels (RGBA32, row-major) of the backdrop at `width` x `height`. Pure; used by tests.
[[nodiscard]] std::vector<std::uint8_t> render_backdrop(const Palette& palette, int width,
                                                        int height);

// Uploads a backdrop texture for a window of `pixel_w` x `pixel_h`.
[[nodiscard]] Result<TexturePtr>
make_backdrop_texture(SDL_Renderer* renderer, const Palette& palette, int pixel_w, int pixel_h);

} // namespace navigator::ui
