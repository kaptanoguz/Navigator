# navigator_commands (owner: commands): CommandQueue, CommandRegistry, built-in command table.
# No renderer, SDL, or ImGui dependency: program layer only.

add_library(navigator_commands STATIC
    command_queue.cpp
    command_registry.cpp
    builtin_commands.cpp
)
add_library(navigator::commands ALIAS navigator_commands)

target_link_libraries(navigator_commands
    PUBLIC
        navigator::headers
    PRIVATE
        navigator::options
)
