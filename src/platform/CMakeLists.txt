# navigator_platform (owner: platform): Linux integration behind include/navigator/watch.hpp,
# volume.hpp, platform.hpp and mime.hpp. Only libc/Linux syscalls (inotify, poll, statvfs, fork/
# exec, /proc, /sys, /dev); no GIO/GVfs, SDL or src/core internals.
# Internal headers in this directory are seams for tests/platform.

add_library(navigator_platform STATIC
    desktop.cpp
    errno_util.cpp
    inotify_watcher.cpp
    launcher.cpp
    mime.cpp
    mountinfo.cpp
    proc_mount_source.cpp
    user_dirs.cpp
    volume_monitor.cpp
    volumes.cpp
)
add_library(navigator::platform ALIAS navigator_platform)

target_link_libraries(navigator_platform
    PUBLIC
        navigator::headers
    PRIVATE
        navigator::options
)
