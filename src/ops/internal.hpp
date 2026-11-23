// src/ops/internal.hpp: entry points shared by engine.cpp and queue.cpp (internal to
// navigator_ops; tests/ops may use them).
#pragma once

#include <functional>
#include <stop_token>

#include <navigator/ops.hpp>
#include <navigator/trash.hpp>

namespace navigator::ops::detail {

// Extra hooks the queue passes to the engine (run_operation() passes none).
struct RunHooks {
    OpId op;                                               // stamped into progress and conflicts
    std::function<bool()> pause_requested;                 // cheap, polled at every check
    std::function<void()> wait_while_paused;               // blocks until resumed or cancelled
    std::function<void(const ItemOutcome&)> item_finished; // top-level item outcomes
};

// Lexical request validation (S3, S13, S15): what submit() and run_operation() reject before
// anything runs. `trash` (may be null) adds ITrash::is_inside_trash to the Trash protection.
[[nodiscard]] Result<void> validate_request(const OpRequest& request,
                                            const ProtectedPaths& protected_paths,
                                            const trash::ITrash* trash);

[[nodiscard]] Result<OpResult> run_operation_ex(const OpRequest& request,
                                                const EngineOptions& options,
                                                const ConflictResolver& resolve,
                                                const ProgressSink& progress, std::stop_token stop,
                                                const RunHooks* hooks);

// The paths an operation touches, for the queue's overlap rule.
[[nodiscard]] std::vector<Path> request_paths(const OpRequest& request);

// OpResult for a request that failed as a whole (every top-level item Failed with `error`) or
// was cancelled before it started (`cancelled`: every item Cancelled).
[[nodiscard]] OpResult failed_result(const OpRequest& request, const Error& error, bool cancelled);

} // namespace navigator::ops::detail
