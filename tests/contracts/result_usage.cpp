// Compile-only check that the Result/fail/ok policy and the CommandQueue drain API are usable
// identically with std::expected and with the navigator::detail fallback. Nothing here runs.
#include <memory>
#include <string>

#include <navigator/commands.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

namespace navigator::contracts_check {

Result<int> parse_positive(int v) {
    if (v <= 0) {
        return fail(ErrorCode::InvalidArgument, "must be positive");
    }
    return v;
}

Result<void> require(bool condition) {
    if (!condition) {
        return fail(ErrorCode::Unsupported);
    }
    return ok();
}

Result<std::string> name_or_error(bool good) {
    if (good) {
        return std::string("ok");
    }
    return fail(ErrorCode::NotFound, "missing");
}

int use_results() {
    int total = 0;
    const Result<int> a = parse_positive(3);
    if (a) {
        total += *a + a.value();
    } else {
        total += static_cast<int>(a.error().code);
    }
    total += parse_positive(-1).value_or(7);
    const Result<void> b = require(true);
    if (!b.has_value()) {
        total += static_cast<int>(b.error().message.size());
    }
    const Result<std::string> c = name_or_error(false);
    total += c ? static_cast<int>(c->size()) : static_cast<int>(to_string(c.error().code).size());
    return total;
}

std::size_t use_queue(CommandQueue& queue) {
    queue.enqueue(CommandId::Quit);
    queue.enqueue(Command{CommandId::ToggleTheme, 1});
    std::size_t seen = 0;
    queue.drain([&](const Command& command) {
        if (command.id == CommandId::Quit) {
            queue.enqueue(CommandId::None); // runs on the next drain
        }
        ++seen;
    });
    return seen + queue.size();
}

int use_app() {
    AppConfig config;
    config.max_frames = 1;
    Result<std::unique_ptr<App>> app = App::create(config);
    if (!app) {
        return 1;
    }
    (*app)->commands().enqueue(CommandId::ToggleFpsOverlay);
    return (*app)->run();
}

} // namespace navigator::contracts_check
