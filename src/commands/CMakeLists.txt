# navigator_commands (owner: commands): CommandQueue, CommandRegistry, built-in command table,
# key chords and the Keymap (with Navigator's default bindings).
# No renderer, SDL, or ImGui dependency: program layer only.

add_library(navigator_commands STATIC
    command_queue.cpp
    command_registry.cpp
    builtin_commands.cpp
    keymap.cpp
    default_keymap.cpp
)
add_library(navigator::commands ALIAS navigator_commands)

target_link_libraries(navigator_commands
    PUBLIC
        navigator::headers
    PRIVATE
        navigator::options
)
