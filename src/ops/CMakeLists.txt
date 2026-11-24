# navigator_ops (owner: ops): Phase 7 file operations for navigator/ops.hpp: the blocking engine
# (run_operation: copy, move, rename, new folder, symlink, trash, restore, confirmed permanent
# delete, undo/redo), the background IOperationQueue, the UndoStack and the pure name / path
# helpers. POSIX/Linux only (renameat2, copy_file_range, FICLONE, SEEK_DATA, xattrs through
# libc). Links navigator_core only: the trash arrives as an injected trash::ITrash, never through
# src/platform. Safety rules S1..S16 (ops.hpp) are normative for every file here.

add_library(navigator_ops STATIC
    archive.cpp
    batch_rename.cpp
    conflict.cpp
    context.cpp
    copy.cpp
    dir_stack.cpp
    engine.cpp
    move.cpp
    names.cpp
    queue.cpp
    simple_ops.cpp
    sys.cpp
    tree.cpp
    undo_exec.cpp
    undo_stack.cpp
)
add_library(navigator::ops ALIAS navigator_ops)

find_package(Threads REQUIRED)
target_link_libraries(navigator_ops
    PUBLIC
        navigator::headers
        navigator::core
        Threads::Threads
    PRIVATE
        navigator::options
)

if(NAVIGATOR_HAVE_LIBARCHIVE)
    target_link_libraries(navigator_ops PRIVATE Navigator::LibArchive)
    target_compile_definitions(navigator_ops PRIVATE NAVIGATOR_HAVE_LIBARCHIVE=1)
else()
    target_compile_definitions(navigator_ops PRIVATE NAVIGATOR_HAVE_LIBARCHIVE=0)
endif()
