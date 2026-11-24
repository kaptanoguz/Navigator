# Sample C11 plugins demonstrating the Navigator C ABI (include/navigator/plugin.h).

add_library(navigator_sample_hello MODULE
    sample_hello/sample_hello.c
)
target_link_libraries(navigator_sample_hello PRIVATE navigator::headers)
set_target_properties(navigator_sample_hello PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sample_hello"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
target_compile_options(navigator_sample_hello PRIVATE -Wall -Wextra -Wpedantic)
