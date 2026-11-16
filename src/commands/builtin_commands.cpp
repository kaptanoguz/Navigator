// Built-in command table, register_builtin_commands(), to_string(CommandId).
//
// Adding a command: append a CommandId in navigator/commands.hpp (never renumber), add a case to
// to_string() and a row to kBuiltins, add its default binding to docs/keymap.md, and add the
// "cmd.<name>" title to the English and Turkish catalogs.
#include <array>
#include <string>

#include <navigator/commands.hpp>

namespace navigator {
namespace {

struct Builtin {
    CommandId id;
    std::string_view name;
    std::string_view title_key;
};

// Order is registration order (and therefore palette order before any sorting).
constexpr std::array kBuiltins{
    Builtin{CommandId::Quit, "app.quit", "cmd.app.quit"},
    Builtin{CommandId::ToggleFpsOverlay, "view.toggle_fps_overlay", "cmd.view.toggle_fps_overlay"},
    Builtin{CommandId::ToggleTheme, "view.toggle_theme", "cmd.view.toggle_theme"},
};

} // namespace

std::string_view to_string(CommandId id) noexcept {
    switch (id) {
    case CommandId::None: return "none";
    case CommandId::Quit: return "app.quit";
    case CommandId::ToggleFpsOverlay: return "view.toggle_fps_overlay";
    case CommandId::ToggleTheme: return "view.toggle_theme";
    }
    return "unknown";
}

Result<void> register_builtin_commands(CommandRegistry& registry) {
    // Validate first so a failure leaves the registry unchanged (no partial registration).
    for (const Builtin& b : kBuiltins) {
        if (registry.find(b.id) != nullptr || registry.find(b.name) != nullptr) {
            return fail(ErrorCode::AlreadyExists,
                        "built-in command already registered: " + std::string(b.name));
        }
    }
    for (const Builtin& b : kBuiltins) {
        if (auto r = registry.add(CommandInfo{b.id, b.name, b.title_key}); !r) {
            return r;
        }
    }
    return ok();
}

} // namespace navigator
