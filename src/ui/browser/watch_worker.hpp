// src/ui/browser/watch_worker.hpp
//
// Watches the browser's current directory off the UI thread. IWatcher has no thread of its own
// and is single-thread-at-a-time, so this worker owns it: it blocks in IWatcher::wait(), drains
// poll(), coalesces a burst (kCoalesce), fs::stat()s created/changed children (blocking I/O, so it
// stays here) and posts one WatchBatch per burst, then calls `wake`.
//
// retarget(dir, tag) moves the watch; batches carry the tag of the target they were produced
// for, so the consumer can tell batches for the directory it shows from stale ones.
// A watcher that cannot be created (factory error) or a directory that cannot be watched simply
// produces no batches: the browser still works, it just does not live-update.
//
// Thread-safety: retarget(), take() and wait_for_batch() may be called from any thread; `wake`
// runs on the worker thread and must be thread-safe.
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
};

class WatchWorker {
public:
    static constexpr std::chrono::milliseconds kCoalesce{60};
    static constexpr std::chrono::milliseconds kPollInterval{100}; // retarget/stop latency

    WatchWorker(WatcherFactory factory, std::function<void()> wake = {});
    ~WatchWorker();
    WatchWorker(const WatchWorker&) = delete;
    WatchWorker& operator=(const WatchWorker&) = delete;
    WatchWorker(WatchWorker&&) = delete;
    WatchWorker& operator=(WatchWorker&&) = delete;

    // Watch `dir` from now on (empty path: watch nothing). Batches are tagged with `tag`.
    void retarget(Path dir, std::uint64_t tag);

    [[nodiscard]] std::vector<WatchBatch> take();

    // Tests: blocks until at least one batch is waiting, or `timeout`.
    [[nodiscard]] bool wait_for_batch(std::chrono::milliseconds timeout);

    // Tests: blocks until the worker has applied the newest retarget() (watch added or failed).
    [[nodiscard]] bool wait_for_target(std::chrono::milliseconds timeout);

private:
    struct Target {
        Path dir;
        std::uint64_t tag = 0;
        std::uint64_t serial = 0;
    };
    void run(const std::stop_token& stop);
    [[nodiscard]] WatchBatch build_batch(const std::vector<WatchEvent>& events, WatchHandle handle,
                                         const Path& dir, std::uint64_t tag) const;

    WatcherFactory factory_;
    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable_any target_cv_;
    std::condition_variable batch_cv_;
    Target target_;             // guarded by mutex_
    std::uint64_t applied_ = 0; // serial of the target the worker has applied
    std::vector<WatchBatch> batches_;
    std::jthread thread_; // last member
};

} // namespace navigator::ui
