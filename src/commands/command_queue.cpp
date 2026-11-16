// CommandQueue implementation. UI-thread only (see navigator/commands.hpp).
#include <navigator/commands.hpp>

namespace navigator {

void CommandQueue::enqueue(Command command) { pending_.push_back(command); }

std::size_t CommandQueue::size() const noexcept { return pending_.size(); }

bool CommandQueue::empty() const noexcept { return pending_.empty(); }

std::vector<Command> CommandQueue::take() {
    std::vector<Command> out;
    out.swap(pending_);
    return out;
}

} // namespace navigator
