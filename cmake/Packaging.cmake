# CPack configuration for Navigator .deb packages (Linux Mint 22 / Ubuntu 24.04+).
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "navigator")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Navigator Contributors")
set(CPACK_PACKAGE_CONTACT "kaptanoguz@users.noreply.github.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast keyboard-driven file manager for Linux")
set(CPACK_PACKAGE_DESCRIPTION
    "Navigator is an independent, fast keyboard-driven file manager for Linux built with C++20/23 and SDL3.")

set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/kaptanoguz/Navigator")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# Allow dpkg-shlibdeps to resolve privately bundled SDL3 when built via FetchContent.
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/sdl3-build")
    list(APPEND CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
        "${CMAKE_BINARY_DIR}/_deps/sdl3-build")
endif()

set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "xdg-utils, bubblewrap")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/packaging/deb/postinst;${CMAKE_SOURCE_DIR}/packaging/deb/postrm")

include(CPack)
