// CommandRegistry implementation. UI-thread only (see navigator/commands.hpp).
//
// Linear lookup: the registry holds tens of commands; a map would cost more than it saves and
// would complicate the registration-order guarantee of all().
#include <algorithm>
#include <string>

#include <navigator/commands.hpp>

namespace navigator {

Result<void> CommandRegistry::add(const CommandInfo& info) {
    if (info.id == CommandId::None) {
        return fail(ErrorCode::InvalidArgument, "command id None cannot be registered");
    }
    if (info.name.empty()) {
        return fail(ErrorCode::InvalidArgument, "command name is empty");
    }
    if (find(info.id) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "command id already registered: " + std::string(to_string(info.id)));
    }
    if (find(info.name) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "command name already registered: " + std::string(info.name));
    }
    commands_.push_back(info);
    return ok();
}

const CommandInfo* CommandRegistry::find(CommandId id) const noexcept {
    const auto it = std::ranges::find(commands_, id, &CommandInfo::id);
    return it == commands_.end() ? nullptr : &*it;
}

const CommandInfo* CommandRegistry::find(std::string_view name) const noexcept {
    const auto it = std::ranges::find(commands_, name, &CommandInfo::name);
    return it == commands_.end() ? nullptr : &*it;
}

Span<const CommandInfo> CommandRegistry::all() const noexcept { return commands_; }

} // namespace navigator
