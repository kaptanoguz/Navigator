// src/ui/platform/outgoing_drag.cpp — picks the outgoing drag backend of the running video driver.
#include "platform/outgoing_drag.hpp"

#include <string_view>

#include <SDL3/SDL.h>

namespace navigator::ui {

std::unique_ptr<OutgoingDrag> create_x11_outgoing_drag(SDL_Window* window);
std::unique_ptr<OutgoingDrag> create_wayland_outgoing_drag(SDL_Window* window);

std::unique_ptr<OutgoingDrag> create_outgoing_drag(SDL_Window* window) {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (window == nullptr || driver == nullptr) {
        return nullptr;
    }
    const std::string_view d(driver);
    if (d == "x11") {
        return create_x11_outgoing_drag(window);
    }
    if (d == "wayland") {
        return create_wayland_outgoing_drag(window);
    }
    return nullptr;
}

} // namespace navigator::ui
