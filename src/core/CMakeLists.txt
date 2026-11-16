# navigator_core (owner: core-fs): directory listing, metadata, name folding/comparison and
# sorting for navigator/fs.hpp. POSIX only; no SDL, GIO or UI dependency.
#
# case_fold_data.inc is generated offline by tools/gen_case_fold.py from the system
# CaseFolding.txt (Ubuntu unicode-data); the build does not regenerate it.

add_library(navigator_core STATIC
    case_fold.cpp
    listing.cpp
    names.cpp
    sort.cpp
)
add_library(navigator::core ALIAS navigator_core)

target_link_libraries(navigator_core
    PUBLIC
        navigator::headers
    PRIVATE
        navigator::options
)
