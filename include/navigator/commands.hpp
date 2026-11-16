// navigator/commands.hpp
//
// Purpose: command identifiers, the per-frame command queue, and the command registry that the
//          keymap and command palette are built on.
// Owner:   contracts (header); implementation in src/commands (role: commands).
// Thread-safety: NOT thread-safe. CommandQueue and CommandRegistry are owned and used by the UI
//                thread only. Worker threads report back through their own channels and the UI
//                thread turns results into commands.
//
// Frame model: widgets never mutate program state while drawing. A user action calls
// CommandQueue::enqueue(); at frame end the app calls drain() and runs every pending command in
// FIFO order. Commands enqueued while a drain is running (by a handler) are NOT run by that drain;
// they stay pending and run on the next drain (normally the next frame).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

// Stable numeric ids: never renumber or reuse a value; append new ids. Values may be persisted
// (keymap files, plugins). Ranges: 1-999 app/view, 1000+ reserved for later phases
// (navigation, selection, file operations, search, ...), added as the phases land.
enum class CommandId : std::uint32_t {
    None = 0,             // "none"       no-op; never registered
    Quit = 1,             // "app.quit"
    ToggleFpsOverlay = 2, // "view.toggle_fps_overlay"
    ToggleTheme = 3,      // "view.toggle_theme"
};

// A queued command. `arg` is a command-specific payload; 0 when unused. Commands that need more
// data than one integer will carry an index/handle into UI-owned state (decided per command).
struct Command {
    CommandId id = CommandId::None;
    std::uint64_t arg = 0;
};

class CommandQueue {
public:
    CommandQueue() = default;

    // Appends to the pending list. Enqueuing CommandId::None is allowed and is a no-op when run.
    void enqueue(Command command);
    void enqueue(CommandId id, std::uint64_t arg = 0) { enqueue(Command{id, arg}); }

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    // Moves all pending commands out (FIFO order) and leaves the queue empty.
    [[nodiscard]] std::vector<Command> take();

    // Calls handler(const Command&) for every command pending at the moment of the call, in FIFO
    // order. Commands the handler enqueues are kept for the next drain.
    template <class F> void drain(F&& handler) {
        const std::vector<Command> batch = take();
        for (const Command& command : batch) {
            handler(command);
        }
    }

private:
    std::vector<Command> pending_;
};

// Static description of a command.
//   name:      stable dotted id, e.g. "app.quit" (keymap files and the palette use it)
//   title_key: i18n key for the palette title, e.g. "cmd.app.quit" (see i18n.hpp)
// Both views must point to storage that outlives the registry (string literals in practice).
struct CommandInfo {
    CommandId id = CommandId::None;
    std::string_view name;
    std::string_view title_key;
};

class CommandRegistry {
public:
    CommandRegistry() = default;

    // Errors: InvalidArgument (id None or empty name), AlreadyExists (duplicate id or name).
    Result<void> add(const CommandInfo& info);

    // nullptr when not registered. Pointers stay valid until the next add().
    [[nodiscard]] const CommandInfo* find(CommandId id) const noexcept;
    [[nodiscard]] const CommandInfo* find(std::string_view name) const noexcept;

    // All commands in registration order. Valid until the next add().
    [[nodiscard]] Span<const CommandInfo> all() const noexcept;

private:
    std::vector<CommandInfo> commands_;
};

// Registers every built-in CommandId (except None) with its name and title_key.
Result<void> register_builtin_commands(CommandRegistry& registry);

// Stable name of a built-in id ("app.quit", ...); "none" for None, "unknown" for other values.
[[nodiscard]] std::string_view to_string(CommandId id) noexcept;

} // namespace navigator
