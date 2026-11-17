// src/ui/sidebar/places_model.hpp
//
// The live data behind the Phase 4 sidebar and the GoTo sources (docs/architecture.md "Phase 4:
// sidebar, GoTo, address bar, bookmarks, recents"): the Places list, the BookmarkStore, the
// RecentsStore, the latest volume snapshot and the Missing probe results, plus their
// persistence timing.
//
//   - VolumeWorker (std::jthread): create_volume_monitor(); wait(250 ms) -> poll() on change;
//     every 10 s while the sidebar is visible a fresh list_volumes() (space usage). Snapshots
//     go to a latest-wins mailbox with a generation; statvfs never runs on the UI thread.
//   - ProbeWorker (std::jthread): fs::stat(path, follow) for each Online location not on a
//     Network volume (a hung share must not stall it); NotFound / not a directory -> Missing.
//     Requests carry a generation; stale results are dropped.
//   - frame_start() (UI thread) takes the latest snapshot, resyncs both stores (saves when
//     they changed), posts a probe request when the set of locations or the volumes changed,
//     and applies finished probe results.
//   - Persistence: bookmarks.json is saved right after each edit (background JobWriter, latest
//     wins); recents.json is debounced 2 s after the last change; both are written
//     synchronously by final_save(). restore = false starts empty, persist = false never
//     writes (AppConfig::restore_session / save_session; the App also clears both for headless
//     --frames runs without --config-dir).
// Headless: no SDL. Tests construct it without workers and inject volumes with set_volumes().
// Thread-safety: UI thread only (the workers have their own threads and mailboxes).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <navigator/places.hpp>
#include <navigator/platform.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>

#include "workspace/session_writer.hpp"

namespace navigator::ui {

class VolumeWorker {
public:
    struct Options {
        VolumeQuery query;
        std::chrono::milliseconds refresh{10000}; // space refresh while enabled
        std::chrono::milliseconds poll{250};      // monitor wait step (also the stop latency)
        std::function<void()> wake;               // a snapshot is ready (any thread)
        std::function<void(const std::string&)> log;
    };
    explicit VolumeWorker(Options options);
    ~VolumeWorker();
    VolumeWorker(const VolumeWorker&) = delete;
    VolumeWorker& operator=(const VolumeWorker&) = delete;
    VolumeWorker(VolumeWorker&&) = delete;
    VolumeWorker& operator=(VolumeWorker&&) = delete;

    // The periodic space refresh runs only while enabled (sidebar visible).
    void set_refresh_enabled(bool enabled);
    // The newest snapshot if its generation is above `seen`.
    [[nodiscard]] std::optional<std::pair<std::uint64_t, std::vector<VolumeInfo>>>
    take(std::uint64_t seen);
    // Tests: blocks until a snapshot with generation > seen exists.
    [[nodiscard]] bool wait(std::uint64_t seen, std::chrono::milliseconds timeout);

private:
    void run(const std::stop_token& stop);
    void publish(std::vector<VolumeInfo> volumes);

    Options options_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::vector<VolumeInfo> latest_;
    std::uint64_t generation_ = 0;
    bool refresh_enabled_ = true;
    std::jthread thread_; // last member
};

class ProbeWorker {
public:
    struct Result {
        std::uint64_t generation = 0;
        std::vector<Path> missing;
    };
    explicit ProbeWorker(std::function<void()> wake);
    ~ProbeWorker();
    ProbeWorker(const ProbeWorker&) = delete;
    ProbeWorker& operator=(const ProbeWorker&) = delete;
    ProbeWorker(ProbeWorker&&) = delete;
    ProbeWorker& operator=(ProbeWorker&&) = delete;

    void request(std::uint64_t generation, std::vector<Path> paths); // latest wins
    [[nodiscard]] std::optional<Result> take();
    [[nodiscard]] bool wait(std::uint64_t generation, std::chrono::milliseconds timeout);

private:
    void run(const std::stop_token& stop);

    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::optional<std::pair<std::uint64_t, std::vector<Path>>> job_;
    std::optional<Result> result_;
    std::uint64_t done_ = 0;
    std::jthread thread_; // last member
};

class PlacesModel {
public:
    struct Options {
        Path home;
        platform::UserDirs user_dirs; // read once at startup by the App (read_user_dirs)
        Path config_root;             // bookmarks.json / recents.json live here
        bool restore = false;         // load the files at load()
        bool persist = false;         // write them
        // Workers. Tests leave volume_worker off and call set_volumes().
        bool volume_worker = false;
        VolumeQuery volume_query;
        bool probe_worker = true;
        std::function<void()> wake;
        std::function<void(const std::string&)> log;
        std::size_t recents_capacity = places::kDefaultRecentsCapacity;
    };

    explicit PlacesModel(Options options);
    ~PlacesModel();
    PlacesModel(const PlacesModel&) = delete;
    PlacesModel& operator=(const PlacesModel&) = delete;
    PlacesModel(PlacesModel&&) = delete;
    PlacesModel& operator=(PlacesModel&&) = delete;

    // Startup (UI thread, small files): loads bookmarks and recents when restore is set.
    void load();

    // ---- Frame start ---------------------------------------------------------------------
    // Applies a new volume snapshot and probe results. True when anything shown changed.
    bool frame_start();

    // ---- State -----------------------------------------------------------------------------
    [[nodiscard]] const std::vector<places::Place>& places() const noexcept { return places_; }
    [[nodiscard]] const places::BookmarkStore& bookmarks() const noexcept { return bookmarks_; }
    [[nodiscard]] const places::RecentsStore& recents() const noexcept { return recents_; }
    [[nodiscard]] const std::vector<VolumeInfo>& volumes() const noexcept { return volumes_; }
    [[nodiscard]] const Path& home() const noexcept { return options_.home; }
    // Status of a stored location: resolve_location() plus the probe's Missing verdict.
    // `effective` (optional) receives the path to navigate to.
    [[nodiscard]] places::LocationStatus status(const Path& path,
                                                const std::optional<VolumeIdentity>& volume,
                                                Path* effective = nullptr) const;
    // The probe found `path` missing (not a directory or gone).
    [[nodiscard]] bool missing(const Path& path) const { return missing_.contains(path); }
    // Increases whenever something the sidebar shows changed.
    [[nodiscard]] std::uint64_t serial() const noexcept { return serial_; }

    // ---- Edits (frame end) -----------------------------------------------------------------
    enum class Toggled : std::uint8_t { Added, Removed, Failed };
    // bookmarks.toggle with arg 0: bookmark `folder` or remove its bookmark.
    Toggled toggle_bookmark(const Path& folder, Timestamp now);
    Result<places::BookmarkId> add_bookmark(const Path& folder, Timestamp now,
                                            std::optional<std::size_t> index = std::nullopt);
    bool remove_bookmark(places::BookmarkId id);
    Result<void> rename_bookmark(places::BookmarkId id, std::string name);
    // One row up (-1) or down (+1), no wrap. False when it could not move.
    bool move_bookmark(places::BookmarkId id, int delta);
    bool move_bookmark_to(places::BookmarkId id, std::size_t index);
    // A folder was listed as a new location (Workspace::take_visits).
    void record_visit(const Path& folder, Timestamp now, double now_ms);
    void clear_recents(double now_ms);
    // Flips RecentsOptions::enabled; returns the new value.
    bool toggle_tracking(double now_ms);

    // The volume snapshot without a worker (tests, and the App before the first snapshot).
    void set_volumes(std::vector<VolumeInfo> volumes);
    // Space refresh only while the sidebar is visible.
    void set_sidebar_visible(bool visible);

    // ---- Persistence -----------------------------------------------------------------------
    // Debounced recents save.
    void tick(double now_ms);
    [[nodiscard]] std::optional<double> save_remaining(double now_ms) const {
        return recents_save_.remaining(now_ms);
    }
    // Quit: waits for queued writes, then writes each file whose store changed during this
    // run synchronously (persist only; an untouched store creates no file).
    void final_save();
    [[nodiscard]] std::uint64_t writes() const;

    // ---- Tests -----------------------------------------------------------------------------
    // Blocks until the newest probe request has a result applied by frame_start() (call it
    // after this). True when no probe is outstanding or it arrived in time.
    [[nodiscard]] bool wait_probe(std::chrono::milliseconds timeout);
    [[nodiscard]] bool wait_volumes(std::chrono::milliseconds timeout);
    [[nodiscard]] bool probe_pending() const noexcept { return probe_wanted_ > probe_applied_; }
    // The first volume snapshot arrived (always true without a volume worker).
    [[nodiscard]] bool volumes_ready() const noexcept {
        return !volume_worker_ || volume_generation_ > 0;
    }

private:
    void apply_volumes(std::vector<VolumeInfo> volumes);
    void request_probe();
    void save_bookmarks();
    void mark_recents(double now_ms);
    void log(const std::string& msg) const;

    Options options_;
    std::vector<places::Place> places_;
    places::BookmarkStore bookmarks_;
    places::RecentsStore recents_;
    std::vector<VolumeInfo> volumes_;
    std::uint64_t volume_generation_ = 0;
    std::set<Path> missing_;
    std::uint64_t serial_ = 1;

    bool probe_dirty_ = true;
    std::uint64_t probe_wanted_ = 0;
    std::uint64_t probe_applied_ = 0;

    SaveScheduler recents_save_;
    bool bookmarks_dirty_ = false; // changed since load (final_save writes only these)
    bool recents_dirty_ = false;
    std::unique_ptr<JobWriter> bookmarks_writer_;
    std::unique_ptr<JobWriter> recents_writer_;
    // Workers last: destroyed first.
    std::unique_ptr<ProbeWorker> prober_;
    std::unique_ptr<VolumeWorker> volume_worker_;
};

} // namespace navigator::ui
