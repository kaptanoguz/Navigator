// src/ui/search/index_service.hpp
//
// Phase 5 name-index service of the App (docs/architecture.md "Name index service"): one
// search::IIndex per default-indexed volume (search::should_index_by_default: local Root, Home
// and writable Internal volumes), created from the Phase 4 volume snapshots, cached under
// <cache root>/index/<stable id>.idx, fed by inotify through the injected WatcherFactory, at
// background priority.
//   - Indexes are created as volumes appear and STARTED only after start() (the App calls it
//     after the first frame was presented, so a cold scan never delays the first paint).
//   - A volume that disappears: its index is stopped on a reaper thread (stop() saves and
//     joins its service thread; never on the UI thread) and dropped. Flattened views that still
//     hold the shared_ptr keep querying the stopped index's data until they are left.
//   - shutdown(budget): stops every index in parallel on helper threads and waits at most
//     `budget`; whatever is still saving after that is detached (quit must not hang).
//   - status changes arrive on the index threads (on_status_change): an atomic flag plus the
//     wake callback; frame_start() reports and clears it.
// With Options::enabled == false (AppConfig::enable_index false: `--no-index`, headless runs
// without --cache-dir) the service never creates an index, reads or writes a file.
// Thread-safety: UI thread only (the indexes own their threads).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <navigator/index.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>
#include <navigator/watch.hpp>

namespace navigator::ui {

class IndexService {
public:
    using Factory =
        std::function<Result<std::unique_ptr<search::IIndex>>(search::NameIndexOptions)>;
    struct Options {
        bool enabled = true;
        Path cache_root; // <cache root>; files go to index_file(cache_root, identity)
        WatcherFactory watcher_factory;
        std::function<void()> wake;
        Factory factory; // empty: search::create_name_index
        std::size_t watch_budget = search::kDefaultWatchBudget;
    };

    explicit IndexService(Options options);
    ~IndexService(); // shutdown(2 s) if not done yet
    IndexService(const IndexService&) = delete;
    IndexService& operator=(const IndexService&) = delete;
    IndexService(IndexService&&) = delete;
    IndexService& operator=(IndexService&&) = delete;

    // A new volume snapshot: creates the indexes of new default volumes, retires the ones whose
    // volume is gone. Returns true when the set changed.
    bool set_volumes(const std::vector<VolumeInfo>& volumes);
    // Development (NAVIGATOR_DEBUG_INDEX_ROOT): index `root` as if it were a volume.
    bool add_root(const Path& root, const VolumeIdentity& identity);
    // Starts every index created so far, and from now on new ones at creation.
    void start();
    [[nodiscard]] bool started() const noexcept { return started_; }

    // The index whose root is the deepest mount point containing `dir`, or nullptr.
    [[nodiscard]] std::shared_ptr<search::IIndex> index_for(const Path& dir) const;
    // search.rebuild_index for the volume holding `dir`. False when it is not indexed.
    bool rebuild(const Path& dir) const;
    // A status changed since the last call (repaint the status bar).
    bool frame_start() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return indexes_.size(); }
    [[nodiscard]] bool enabled() const noexcept { return options_.enabled; }

    // Stops everything within `budget` (see above). Idempotent.
    void shutdown(std::chrono::milliseconds budget);

private:
    struct Entry {
        Path root;
        std::shared_ptr<search::IIndex> index;
    };
    bool create(const Path& root, const VolumeIdentity& identity, const std::string& id);
    void retire(std::shared_ptr<search::IIndex> index);

    Options options_;
    std::map<std::string, Entry> indexes_; // by index_stable_id
    std::shared_ptr<std::atomic<bool>> changed_ = std::make_shared<std::atomic<bool>>(false);
    std::vector<std::thread> reapers_;
    bool started_ = false;
    bool shut_down_ = false;
};

} // namespace navigator::ui
