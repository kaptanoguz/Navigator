// FPS overlay: render backend names shown to the user.
#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>

#include "render/backend_name.hpp"

using navigator::Language;
using navigator::set_language;
using navigator::ui::backend_display_name;

TEST_CASE("Backend names are readable", "[render]") {
    set_language(Language::English);
    CHECK(backend_display_name("gpu", "vulkan") == "Vulkan (SDL_GPU)");
    CHECK(backend_display_name("gpu", "") == "SDL_GPU");
    CHECK(backend_display_name("gpu", "newthing") == "newthing (SDL_GPU)");
    CHECK(backend_display_name("opengl", "") == "OpenGL");
    CHECK(backend_display_name("opengles2", "") == "OpenGL ES 2");
    CHECK(backend_display_name("vulkan", "") == "Vulkan");
    CHECK(backend_display_name("software", "") == "Software");
    CHECK(backend_display_name("", "") == "Unknown");
    CHECK(backend_display_name("custom", "") == "custom");
}

TEST_CASE("Software backend name is translated", "[render][i18n]") {
    set_language(Language::Turkish);
    CHECK(backend_display_name("software", "") == "Yazılım");
    CHECK(backend_display_name("gpu", "vulkan") == "Vulkan (SDL_GPU)");
    set_language(Language::English);
}
