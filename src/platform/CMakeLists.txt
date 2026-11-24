# navigator_platform (owner: platform): Linux integration behind include/navigator/watch.hpp,
# volume.hpp, platform.hpp, mime.hpp, trash.hpp and transfer.hpp. Only libc/Linux syscalls
# (inotify, poll, statvfs, fork/exec, rename, /proc, /sys, /dev); no GIO/GVfs, SDL or src/core.
# Internal headers in this directory are seams for tests/platform.

add_library(navigator_platform STATIC
    desktop.cpp
    errno_util.cpp
    inotify_watcher.cpp
    launcher.cpp
    mime.cpp
    mountinfo.cpp
    openwith_assoc.cpp
    openwith_desktop.cpp
    process_runner.cpp
    transfer.cpp
    trash.cpp
    trash_format.cpp
    program_dirs.cpp
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
