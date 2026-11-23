# Navigator root build. Skeleton owned by `packaging`; the coordinator performs
# the final integration pass (see docs/integration-notes.md).
cmake_minimum_required(VERSION 3.25)

project(Navigator
    VERSION 0.0.1
    DESCRIPTION "Independent Linux file manager"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language settings: prefer C++23, fall back to C++20 when unavailable.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NAVIGATOR_BUILD_TESTS "Build Navigator tests" ON)
option(NAVIGATOR_BUILD_APP "Build the navigator executable" ON)
option(NAVIGATOR_WERROR "Treat warnings as errors on Navigator targets" OFF)
option(NAVIGATOR_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(NAVIGATOR_FETCH_DEPS "Allow FetchContent for SDL3/Catch2 when not found" ON)

include(NavigatorOptions)   # navigator_options / navigator::options
include(Dependencies)       # SDL3::SDL3, Catch2::Catch2WithMain, Freetype, harfbuzz, Fontconfig

# ---------------------------------------------------------------------------
# Public headers (include/navigator). Header-only interface target.
# ---------------------------------------------------------------------------
add_library(navigator_headers INTERFACE)
add_library(navigator::headers ALIAS navigator_headers)
target_include_directories(navigator_headers INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_features(navigator_headers INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# Modules. Each guarded so the tree configures while modules are in flight.
# ---------------------------------------------------------------------------
function(navigator_add_subdir dir)
    if(EXISTS "${PROJECT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory("${dir}")
    else()
        message(STATUS "Navigator: skipping ${dir} (no CMakeLists.txt yet)")
    endif()
endfunction()

if(NAVIGATOR_BUILD_TESTS)
    enable_testing() # before any subdirectory that registers tests (e.g. tools/)
endif()

navigator_add_subdir(src/core)
navigator_add_subdir(src/platform)
navigator_add_subdir(src/search)
navigator_add_subdir(src/preview)
navigator_add_subdir(src/commands)
navigator_add_subdir(src/ui)
if(NAVIGATOR_BUILD_APP)
    navigator_add_subdir(src/app)
    navigator_add_subdir(tools)
endif()

if(NAVIGATOR_BUILD_TESTS)
    enable_testing()
    navigator_add_subdir(tests/contracts)
    navigator_add_subdir(tests/core)
    navigator_add_subdir(tests/platform)
    navigator_add_subdir(tests/search)
    navigator_add_subdir(tests/preview)
    navigator_add_subdir(tests/integration)
    navigator_add_subdir(tests/commands)
    navigator_add_subdir(tests/ui)
    navigator_add_subdir(tests/smoke)   # after src/app: smoke may use the navigator target
endif()

# ---------------------------------------------------------------------------
# Install (guarded; the app target is defined in src/app).
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
if(TARGET navigator)
    install(TARGETS navigator RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/navigator.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()
