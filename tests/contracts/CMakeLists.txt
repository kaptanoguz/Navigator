# Header compile test (owner: contracts).
#
# Every public header is compiled alone in its own generated translation unit, in C++20 and
# C++23, with -Wall -Wextra -Wpedantic -Werror. plugin.h is also compiled as C11. All object
# libraries are part of ALL and are grouped under the `navigator_header_check` target.
#
# Expects the root project to define the INTERFACE target `navigator_headers`
# (include directory: ${PROJECT_SOURCE_DIR}/include).

if(NOT TARGET navigator_headers)
    message(FATAL_ERROR "tests/contracts requires the INTERFACE target navigator_headers")
endif()

enable_language(C)

set(NAVIGATOR_CONTRACT_HEADERS
    types
    fs
    watch
    volume
    index
    search
    ops
    preview
    thumbnail
    preview_service
    commands
    keymap_file
    palette
    menu
    openwith
    ui
    platform
    mime
    i18n
    layout
    places
    path_input
    walk
    trash
    transfer
    view_settings
    batch_rename
    appearance
    plugins
    archive
    content_search
    git
    detail/expected
)

set(_nav_warnings -Wall -Wextra -Wpedantic -Werror)
set(_nav_check_targets "")

function(_navigator_header_object name source lang std)
    add_library(${name} OBJECT ${source})
    target_link_libraries(${name} PRIVATE navigator_headers)
    target_compile_options(${name} PRIVATE ${_nav_warnings})
    if(lang STREQUAL "C")
        set_target_properties(${name} PROPERTIES C_STANDARD ${std} C_STANDARD_REQUIRED ON
                                                 C_EXTENSIONS OFF)
    else()
        set_target_properties(${name} PROPERTIES CXX_STANDARD ${std} CXX_STANDARD_REQUIRED ON
                                                 CXX_EXTENSIONS OFF)
    endif()
endfunction()

foreach(std IN ITEMS 20 23)
    foreach(header IN LISTS NAVIGATOR_CONTRACT_HEADERS)
        string(REPLACE "/" "_" stem "${header}")
        set(src "${CMAKE_CURRENT_BINARY_DIR}/cxx${std}/check_${stem}.cpp")
        file(GENERATE OUTPUT "${src}" CONTENT "#include <navigator/${header}.hpp>\n")
        _navigator_header_object(navigator_hdr_${stem}_cxx${std} "${src}" CXX ${std})
        list(APPEND _nav_check_targets navigator_hdr_${stem}_cxx${std})
    endforeach()

    # plugin.h from C++
    set(src "${CMAKE_CURRENT_BINARY_DIR}/cxx${std}/check_plugin_h.cpp")
    file(GENERATE OUTPUT "${src}" CONTENT "#include <navigator/plugin.h>\n")
    _navigator_header_object(navigator_hdr_plugin_h_cxx${std} "${src}" CXX ${std})
    list(APPEND _nav_check_targets navigator_hdr_plugin_h_cxx${std})

    # Usage of Result / CommandQueue / App signatures (compile only, never linked).
    _navigator_header_object(navigator_contract_usage_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/result_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_usage_cxx${std})

    # Phase 1 fs/watch/volume usage and test fakes (compile only, never linked).
    _navigator_header_object(navigator_contract_phase1_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase1_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase1_cxx${std})

    # Phase 2 commands/keymap, platform launcher seam, mime, AppConfig (compile only).
    _navigator_header_object(navigator_contract_phase2_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase2_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase2_cxx${std})

    # Phase 3 layout model, session JSON/files, tab/pane commands, AppConfig (compile only).
    _navigator_header_object(navigator_contract_phase3_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase3_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase3_cxx${std})

    # Phase 4 places/bookmarks/recents, path_input (GoTo), user dirs, sidebar session state,
    # sidebar/bookmark/copy-path commands (compile only).
    _navigator_header_object(navigator_contract_phase4_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase4_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase4_cxx${std})

    # Phase 5 walk (flatten enumerator), search (matcher, ranking, type filter, filter
    # pipeline), index (IIndex fake, default volumes, stable ids), TabState type filter,
    # AppConfig index fields, search command ids (compile only).
    _navigator_header_object(navigator_contract_phase5_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase5_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase5_cxx${std})

    # Phase 6 preview values and helpers, a fake provider and registry, Freedesktop thumbnail
    # keys / read / write / policy, thumbnailer parsing and running through a fake
    # IProcessRunner, a fake IPreviewService, InspectorState, AppConfig preview fields,
    # Inspector command ids (compile only).
    _navigator_header_object(navigator_contract_phase6_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase6_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase6_cxx${std})

    # Phase 7 ops requests/conflicts/progress/results, run_operation, IOperationQueue,
    # UndoStack, pure name/path helpers, the Trash service and .trashinfo helpers (with a fake
    # ITrash), clipboard / uri-list / drop-action helpers, new ErrorCodes, file-operation
    # command ids (compile only).
    _navigator_header_object(navigator_contract_phase7_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase7_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase7_cxx${std})

    # Phase 8 key sequences and the SequenceMatcher, command metadata (category, danger, arg
    # kinds, context flags), keymap file model / JSON / layering / editor helpers, palette entries
    # / ranking / history, menu model / pins / filter, Open With handlers / Exec expansion /
    # launching through a fake IAppLauncher, Phase 8 command ids (compile only).
    _navigator_header_object(navigator_contract_phase8_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase8_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase8_cxx${std})

    # Phase 9A view settings, zoom axis, column reorder, hierarchical store, commands (compile only).
    _navigator_header_object(navigator_contract_phase9a_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase9a_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase9a_cxx${std})

    # Phase 9B batch rename engine, appearance settings, custom themes, commands (compile only).
    _navigator_header_object(navigator_contract_phase9b_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase9b_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase9b_cxx${std})

    # Phase 10 GVfs, IPC, terminal, archive, content search, git status, C ABI plugins (compile only).
    _navigator_header_object(navigator_contract_phase10_cxx${std}
                             "${CMAKE_CURRENT_SOURCE_DIR}/phase10_usage.cpp" CXX ${std})
    list(APPEND _nav_check_targets navigator_contract_phase10_cxx${std})
endforeach()

# plugin.h from C11
set(src "${CMAKE_CURRENT_BINARY_DIR}/c11/check_plugin_h.c")
file(GENERATE OUTPUT "${src}" CONTENT "#include <navigator/plugin.h>\n")
_navigator_header_object(navigator_hdr_plugin_h_c11 "${src}" C 11)
list(APPEND _nav_check_targets navigator_hdr_plugin_h_c11)

add_custom_target(navigator_header_check)
add_dependencies(navigator_header_check ${_nav_check_targets})
