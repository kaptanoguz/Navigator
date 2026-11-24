# navigator_plugins (owner: plugins): C ABI plugin loader (dlopen/dlsym) behind
# include/navigator/plugin.h and include/navigator/plugins.hpp.

add_library(navigator_plugins STATIC
    plugin_manager.cpp
)
add_library(navigator::plugins ALIAS navigator_plugins)

target_link_libraries(navigator_plugins
    PUBLIC
        navigator::headers
    PRIVATE
        navigator::options
        ${CMAKE_DL_LIBS}
)
