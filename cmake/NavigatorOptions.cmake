# navigator_options: INTERFACE target carrying warnings / sanitizers for
# Navigator's own targets only. Third-party targets (SDL3, Catch2) never link
# it, so their warnings cannot trip -Werror.
#
# Usage in a module:  target_link_libraries(<tgt> PRIVATE navigator::options)

add_library(navigator_options INTERFACE)
add_library(navigator::options ALIAS navigator_options)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(navigator_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(NAVIGATOR_WERROR)
        target_compile_options(navigator_options INTERFACE -Werror)
    endif()

    if(NAVIGATOR_SANITIZE)
        target_compile_options(navigator_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
        target_link_options(navigator_options INTERFACE -fsanitize=address,undefined)
    endif()
elseif(MSVC)
    target_compile_options(navigator_options INTERFACE /W4 $<$<BOOL:${NAVIGATOR_WERROR}>:/WX>)
endif()
