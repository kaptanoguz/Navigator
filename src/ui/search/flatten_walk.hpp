// src/ui/search/flatten_walk.hpp
//
// Phase 5 flatten (Ctrl+E): one recursive walk on its own thread, streaming batches to the UI
// thread as immutable FlatChunks (docs/architecture.md "Phase 5", "Data flow").
//
// Detach-safe by construction (walk.hpp "Cancellation latency"): a walker can block in a
// syscall that no stop token interrupts, so the UI thread NEVER joins this thread. The thread
// shares only a heap block (shared_ptr) with its owner; cancel() / the destructor request stop,
// drop the wake callback and detach. A cancelled walk exits on its own (within one directory
// chunk on a local filesystem) and frees the shared block. live_threads() / wait_for_exit()
// let the App give running walks a bounded grace period at quit.
//
// The walker itself is opened ON the thread (Walker::open does an open + fstat), through an
// injectable factory so tests stream fake batches without touching the disk.
// Thread-safety: the FlattenWalk object belongs to the UI thread; take() / stats are guarded by
// the shared block's mutex. `wake` runs on the walk thread (SDL_PushEvent is thread-safe).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include <navigator/types.hpp>
#include <navigator/walk.hpp>

#include "search/organize.hpp"

namespace navigator::ui {

// What the walk thread drives: fs::Walker in the app, a scripted fake in tests.
class WalkSource {
public:
    WalkSource() = default;
    WalkSource(const WalkSource&) = delete;
    WalkSource& operator=(const WalkSource&) = delete;
    WalkSource(WalkSource&&) = delete;
    WalkSource& operator=(WalkSource&&) = delete;
    virtual ~WalkSource() = default;
    // As fs::Walker::next_batch.
    [[nodiscard]] virtual Result<bool> next_batch(fs::WalkBatch& out, std::stop_token stop) = 0;
    [[nodiscard]] virtual const fs::WalkStats& stats() const noexcept = 0;
};
using WalkFactory = std::function<Result<std::unique_ptr<WalkSource>>(const fs::WalkOptions&)>;

// fs::Walker::open behind the WalkSource interface.
[[nodiscard]] WalkFactory default_walk_factory();

class FlattenWalk {
public:
    struct Update {
        std::vector<FlatChunkPtr> chunks; // in order; dense ids continue across updates
        fs::WalkStats stats;
        bool finished = false;      // the walk ended: done, truncated, failed or cancelled
        bool cancelled = false;     // ended by cancel()
        std::optional<Error> error; // open failure or a walk error other than Cancelled
    };

    FlattenWalk(fs::WalkOptions options, WalkFactory factory, std::function<void()> wake);
    // cancel() + detach: never blocks.
    ~FlattenWalk();
    FlattenWalk(const FlattenWalk&) = delete;
    FlattenWalk& operator=(const FlattenWalk&) = delete;
    FlattenWalk(FlattenWalk&&) = delete;
    FlattenWalk& operator=(FlattenWalk&&) = delete;

    // Requests stop. Rows already delivered stay valid; take() reports finished + cancelled
    // once the thread has noticed (or immediately when it is blocked: `cancelled()` is set at
    // once and the UI treats the walk as stopped).
    void cancel() noexcept;
    [[nodiscard]] bool cancelled() const noexcept;
    // Chunks and progress since the last call.
    [[nodiscard]] Update take();
    // The thread has returned (tests; the App's quit grace period).
    [[nodiscard]] bool exited() const noexcept;
    // Blocks until the walk finished (tests only; never on the UI thread of the app).
    [[nodiscard]] bool wait_finished(std::chrono::milliseconds timeout) const;

    // Walk threads that have not returned yet, process-wide.
    [[nodiscard]] static int live_threads() noexcept;
    // Polls live_threads() until 0 or `timeout` (quit). True when none is left.
    [[nodiscard]] static bool wait_for_exit(std::chrono::milliseconds timeout) noexcept;

    // Implementation detail shared with the walk thread (flatten_walk.cpp).
    struct Shared;

private:
    std::shared_ptr<Shared> shared_;
};

} // namespace navigator::ui
