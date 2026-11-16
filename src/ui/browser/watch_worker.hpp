// src/ui/browser/watch_worker.hpp
//
// Watches the browser's folders off the UI thread. IWatcher has no thread of its own
// and is single-thread-at-a-time, so this worker owns it: it blocks in IWatcher::wait(), drains
// poll(), coalesces a burst (kCoalesce), fs::stat()s created/changed children (blocking I/O, so it
// stays here) and posts one WatchBatch per burst, then calls `wake`.
//
// Targets are identified by a tag chosen by the caller. watch(dir, tag) adds one and unwatch(tag)
// drops it; both take effect at once (an eventfd wakes the worker out of its wait). The browser
// keeps the shown folder's watch while a navigation is pending and adds the target's watch beside
// it, so a failed navigation never leaves the shown folder unwatched. Every watch() posts a
// readiness marker once IWatcher::add() returned; every batch carries its target's tag.
// A watcher that cannot be created (factory error) or a directory that cannot be watched simply
// produces no batches: the browser still works, it just does not live-update.
//
// Thread-safety: watch(), unwatch(), take() and the wait_* functions may be called from any thread;
// `wake` runs on the worker thread and must be thread-safe.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/watch.hpp>

namespace navigator::ui {

// One change to apply to the listing, in event order.
struct WatchChange {
    enum class Kind : std::uint8_t {
        Upsert, // `entry` (fresh fs::stat) appeared or changed
        Remove, // `name` is gone
        Rename, // `old_name` became `name`; `entry` is the fresh stat of the new name when known
    };
    Kind kind = Kind::Upsert;
    std::string name;
    std::string old_name;
    std::optional<fs::DirEntry> entry;
};

struct WatchBatch {
    std::uint64_t tag = 0;
    std::vector<WatchChange> changes;
    bool overflow = false; // events were lost: re-list
    bool gone = false;     // the watched directory itself was deleted/moved/unmounted
    // Readiness marker, posted once per watch() after IWatcher::add() returned: `watching`
    // tells whether the watch is in place, `at` when add() returned. Changes before `at` are
    // not reported, so a listing that began earlier must be redone.
    bool ready = false;
    bool watching = false;
    std::chrono::steady_clock::time_point at{};
};

class WatchWorker {
public:
    static constexpr std::chrono::milliseconds kCoalesce{60};
    // Retarget/stop latency for watchers without a pollable fd (fakes); the inotify backend is
    // woken immediately through an eventfd.
    static constexpr std::chrono::milliseconds kPollInterval{100};

    WatchWorker(WatcherFactory factory, std::function<void()> wake = {});
    ~WatchWorker();
    WatchWorker(const WatchWorker&) = delete;
    WatchWorker& operator=(const WatchWorker&) = delete;
    WatchWorker(WatchWorker&&) = delete;
    WatchWorker& operator=(WatchWorker&&) = delete;

    // Adds a watch on `dir` whose batches carry `tag` (tags must increase across watch() calls).
    void watch(Path dir, std::uint64_t tag);
    // Drops the watch with `tag` (unknown tags are ignored). Its pending events are discarded.
    void unwatch(std::uint64_t tag);

    [[nodiscard]] std::vector<WatchBatch> take();

    // Tests: blocks until at least one batch is waiting, or `timeout`.
    [[nodiscard]] bool wait_for_batch(std::chrono::milliseconds timeout);

    // Blocks until the worker has applied watch(.., `tag`) (or a later one), or `timeout`.
    [[nodiscard]] bool wait_for_tag(std::uint64_t tag, std::chrono::milliseconds timeout);

    // Tests: blocks until the worker has applied every watch()/unwatch() so far.
    [[nodiscard]] bool wait_for_target(std::chrono::milliseconds timeout);

private:
    struct Op {
        bool add = true;
        Path dir;
        std::uint64_t tag = 0;
    };
    struct Target {
        std::uint64_t tag = 0;
        Path dir;
        WatchHandle handle = kInvalidWatchHandle;
    };
    void run(const std::stop_token& stop);
    void signal() const noexcept; // wakes run() out of its poll(2)
    // Waits until the watcher has events (true), or a watch()/unwatch()/stop signal or `timeout`
    // (negative: no limit) passes (false).
    [[nodiscard]] bool wait_events(IWatcher& watcher, std::chrono::milliseconds timeout) const;
    [[nodiscard]] WatchBatch build_batch(const std::vector<WatchEvent>& events, WatchHandle handle,
                                         const Path& dir, std::uint64_t tag) const;

    WatcherFactory factory_;
    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable_any target_cv_;
    std::condition_variable batch_cv_;
    std::vector<Op> ops_;           // guarded by mutex_
    std::uint64_t queued_ = 0;      // ops queued so far
    std::uint64_t applied_ = 0;     // ops applied by the worker
    std::uint64_t applied_tag_ = 0; // newest tag a watch() was applied for
    std::vector<WatchBatch> batches_;
    int wake_fd_ = -1;    // eventfd; -1 when unavailable (falls back to kPollInterval)
    std::jthread thread_; // last member
};

} // namespace navigator::ui
