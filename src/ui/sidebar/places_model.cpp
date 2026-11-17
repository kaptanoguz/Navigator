// src/ui/sidebar/places_model.cpp — see places_model.hpp.
#include "sidebar/places_model.hpp"

#include <algorithm>

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>

namespace navigator::ui {

// ---------------------------------------------------------------------------------------------
// VolumeWorker
// ---------------------------------------------------------------------------------------------

VolumeWorker::VolumeWorker(Options options)
    : options_(std::move(options)), thread_([this](const std::stop_token& stop) { run(stop); }) {}

VolumeWorker::~VolumeWorker() {
    thread_.request_stop();
    cv_.notify_all();
}

void VolumeWorker::set_refresh_enabled(bool enabled) {
    const std::scoped_lock lock(mutex_);
    refresh_enabled_ = enabled;
}

std::optional<std::pair<std::uint64_t, std::vector<VolumeInfo>>>
VolumeWorker::take(std::uint64_t seen) {
    const std::scoped_lock lock(mutex_);
    if (generation_ <= seen) {
        return std::nullopt;
    }
    return std::make_pair(generation_, latest_);
}

bool VolumeWorker::wait(std::uint64_t seen, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return generation_ > seen; });
}

void VolumeWorker::publish(std::vector<VolumeInfo> volumes) {
    {
        const std::scoped_lock lock(mutex_);
        latest_ = std::move(volumes);
        ++generation_;
    }
    cv_.notify_all();
    if (options_.wake) {
        options_.wake();
    }
}

void VolumeWorker::run(const std::stop_token& stop) {
    Result<std::unique_ptr<IVolumeMonitor>> monitor = create_volume_monitor(options_.query);
    if (!monitor) {
        if (options_.log) {
            options_.log("volume monitor unavailable: " + monitor.error().message);
        }
    } else {
        publish((*monitor)->volumes());
    }
    auto last_refresh = std::chrono::steady_clock::now();
    std::vector<VolumeEvent> events;
    while (!stop.stop_requested()) {
        bool signalled = false;
        if (monitor) {
            signalled = (*monitor)->wait(options_.poll);
        } else {
            std::unique_lock lock(mutex_);
            (void)cv_.wait_for(lock, stop, options_.poll, [] { return false; });
        }
        if (stop.stop_requested()) {
            return;
        }
        if (signalled && monitor) {
            events.clear();
            const Result<std::size_t> n = (*monitor)->poll(events);
            if (n && *n > 0) {
                publish((*monitor)->volumes());
                last_refresh = std::chrono::steady_clock::now();
            }
        }
        bool refresh = false;
        {
            const std::scoped_lock lock(mutex_);
            refresh = refresh_enabled_;
        }
        const auto now = std::chrono::steady_clock::now();
        if (refresh && now - last_refresh >= options_.refresh) {
            last_refresh = now;
            Result<std::vector<VolumeInfo>> fresh = list_volumes(options_.query);
            if (fresh) {
                publish(std::move(*fresh));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// ProbeWorker
// ---------------------------------------------------------------------------------------------

ProbeWorker::ProbeWorker(std::function<void()> wake)
    : wake_(std::move(wake)), thread_([this](const std::stop_token& stop) { run(stop); }) {}

ProbeWorker::~ProbeWorker() {
    thread_.request_stop();
    cv_.notify_all();
}

void ProbeWorker::request(std::uint64_t generation, std::vector<Path> paths) {
    {
        const std::scoped_lock lock(mutex_);
        job_ = std::make_pair(generation, std::move(paths));
    }
    cv_.notify_all();
}

std::optional<ProbeWorker::Result> ProbeWorker::take() {
    const std::scoped_lock lock(mutex_);
    std::optional<Result> out;
    out.swap(result_);
    return out;
}

bool ProbeWorker::wait(std::uint64_t generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return done_ >= generation; });
}

void ProbeWorker::run(const std::stop_token& stop) {
    while (true) {
        std::pair<std::uint64_t, std::vector<Path>> job;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait(lock, stop, [this] { return job_.has_value(); })) {
                return;
            }
            if (job_) {
                job = std::move(*job_);
            }
            job_.reset();
        }
        Result out;
        out.generation = job.first;
        for (const Path& p : job.second) {
            if (stop.stop_requested()) {
                return;
            }
            const navigator::Result<fs::DirEntry> st = fs::stat(p, true);
            if (st ? !fs::is_directory_like(*st) : st.error().code == ErrorCode::NotFound) {
                out.missing.push_back(p);
            }
        }
        {
            const std::scoped_lock lock(mutex_);
            // A newer job queued meanwhile supersedes this result.
            if (!job_.has_value()) {
                result_ = std::move(out);
            }
            done_ = std::max(done_, job.first);
        }
        cv_.notify_all();
        if (wake_) {
            wake_();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// PlacesModel
// ---------------------------------------------------------------------------------------------

PlacesModel::PlacesModel(Options options)
    : options_(std::move(options)), recents_(options_.recents_capacity) {
    places_ = places::build_places(options_.home, options_.user_dirs);
    if (options_.persist && !options_.config_root.empty()) {
        bookmarks_writer_ = std::make_unique<JobWriter>();
        recents_writer_ = std::make_unique<JobWriter>();
    }
    if (options_.probe_worker) {
        prober_ = std::make_unique<ProbeWorker>(options_.wake);
    }
    if (options_.volume_worker) {
        VolumeWorker::Options vo;
        vo.query = options_.volume_query;
        vo.wake = options_.wake;
        vo.log = options_.log;
        volume_worker_ = std::make_unique<VolumeWorker>(std::move(vo));
    }
}

PlacesModel::~PlacesModel() = default;

void PlacesModel::log(const std::string& msg) const {
    if (options_.log) {
        options_.log(msg);
    }
}

void PlacesModel::load() {
    if (!options_.restore || options_.config_root.empty()) {
        return;
    }
    if (Result<places::BookmarkStore> b =
            places::load_bookmarks(places::bookmarks_file(options_.config_root));
        b) {
        bookmarks_ = std::move(*b);
    } else if (b.error().code != ErrorCode::NotFound) {
        log("bookmarks not loaded: " + b.error().message);
    }
    if (Result<places::RecentsStore> r = places::load_recents(
            places::recents_file(options_.config_root), options_.recents_capacity);
        r) {
        recents_ = std::move(*r);
    } else if (r.error().code != ErrorCode::NotFound) {
        log("recents not loaded: " + r.error().message);
    }
    probe_dirty_ = true;
    ++serial_;
}

bool PlacesModel::frame_start() {
    bool changed = false;
    if (volume_worker_) {
        if (auto snap = volume_worker_->take(volume_generation_); snap) {
            volume_generation_ = snap->first;
            apply_volumes(std::move(snap->second));
            changed = true;
        }
    }
    if (prober_) {
        if (std::optional<ProbeWorker::Result> r = prober_->take(); r) {
            if (r->generation == probe_wanted_) {
                std::set<Path> next(r->missing.begin(), r->missing.end());
                if (next != missing_) {
                    missing_ = std::move(next);
                    ++serial_;
                    changed = true;
                }
                probe_applied_ = r->generation;
            }
        }
    }
    if (probe_dirty_) {
        request_probe();
    }
    return changed;
}

void PlacesModel::apply_volumes(std::vector<VolumeInfo> volumes) {
    volumes_ = std::move(volumes);
    if (bookmarks_.resync(volumes_) > 0) {
        save_bookmarks();
    }
    if (recents_.resync(volumes_) > 0) {
        // No clock here: the next tick() saves it with the regular debounce.
        recents_dirty_ = true;
        recents_save_.mark(0.0);
    }
    probe_dirty_ = true;
    ++serial_;
}

void PlacesModel::set_volumes(std::vector<VolumeInfo> volumes) {
    apply_volumes(std::move(volumes));
}

void PlacesModel::set_sidebar_visible(bool visible) {
    if (volume_worker_) {
        volume_worker_->set_refresh_enabled(visible);
    }
}

places::LocationStatus PlacesModel::status(const Path& path,
                                           const std::optional<VolumeIdentity>& volume,
                                           Path* effective) const {
    const places::ResolvedLocation r = places::resolve_location(path, volume, volumes_);
    if (effective != nullptr) {
        *effective = r.path;
    }
    if (r.status == places::LocationStatus::Online && missing_.contains(r.path)) {
        return places::LocationStatus::Missing;
    }
    return r.status;
}

void PlacesModel::request_probe() {
    probe_dirty_ = false;
    if (!prober_) {
        return;
    }
    std::vector<Path> paths;
    const auto add = [&](const Path& p, const std::optional<VolumeIdentity>& id) {
        const places::ResolvedLocation r = places::resolve_location(p, id, volumes_);
        if (r.status != places::LocationStatus::Online) {
            return;
        }
        const std::optional<std::size_t> v =
            r.volume_index ? r.volume_index : places::containing_volume(r.path, volumes_);
        if (v && *v < volumes_.size() && volumes_[*v].kind == VolumeKind::Network) {
            return; // never stat a share that may hang
        }
        paths.push_back(r.path);
    };
    for (const places::Place& p : places_) {
        add(p.path, std::nullopt);
    }
    for (const places::Bookmark& b : bookmarks_.list()) {
        add(b.path, b.volume);
    }
    for (const places::RecentEntry& e : recents_.entries()) {
        add(e.path, e.volume);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    ++probe_wanted_;
    prober_->request(probe_wanted_, std::move(paths));
}

bool PlacesModel::wait_probe(std::chrono::milliseconds timeout) {
    if (!prober_ || !probe_pending()) {
        return true;
    }
    return prober_->wait(probe_wanted_, timeout);
}

bool PlacesModel::wait_volumes(std::chrono::milliseconds timeout) {
    if (!volume_worker_) {
        return true;
    }
    return volume_worker_->wait(volume_generation_, timeout);
}

// ---------------------------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------------------------

void PlacesModel::save_bookmarks() {
    ++serial_;
    bookmarks_dirty_ = true;
    probe_dirty_ = true;
    if (!bookmarks_writer_) {
        return;
    }
    bookmarks_writer_->submit([file = places::bookmarks_file(options_.config_root),
                               copy = bookmarks_, log = options_.log]() -> Result<void> {
        Result<void> r = places::save_bookmarks(file, copy);
        if (!r && log) {
            log("bookmarks save failed: " + r.error().message);
        }
        return r;
    });
}

void PlacesModel::mark_recents(double now_ms) {
    ++serial_;
    recents_dirty_ = true;
    probe_dirty_ = true;
    recents_save_.mark(now_ms);
}

PlacesModel::Toggled PlacesModel::toggle_bookmark(const Path& folder, Timestamp now) {
    const Path p = path_input::normalize_absolute(folder);
    if (const places::Bookmark* b = bookmarks_.find(p); b != nullptr) {
        return remove_bookmark(b->id) ? Toggled::Removed : Toggled::Failed;
    }
    return add_bookmark(p, now) ? Toggled::Added : Toggled::Failed;
}

Result<places::BookmarkId> PlacesModel::add_bookmark(const Path& folder, Timestamp now,
                                                     std::optional<std::size_t> index) {
    const Path p = path_input::normalize_absolute(folder);
    Result<places::BookmarkId> id =
        bookmarks_.add(p, places::identity_for(p, volumes_), now, {}, index);
    if (id) {
        save_bookmarks();
    }
    return id;
}

bool PlacesModel::remove_bookmark(places::BookmarkId id) {
    if (!bookmarks_.remove(id)) {
        return false;
    }
    save_bookmarks();
    return true;
}

Result<void> PlacesModel::rename_bookmark(places::BookmarkId id, std::string name) {
    Result<void> r = bookmarks_.rename(id, std::move(name));
    if (r) {
        save_bookmarks();
    }
    return r;
}

bool PlacesModel::move_bookmark(places::BookmarkId id, int delta) {
    const std::optional<std::size_t> at = bookmarks_.index_of(id);
    if (!at) {
        return false;
    }
    if ((delta < 0 && *at == 0) || (delta > 0 && *at + 1 >= bookmarks_.size()) || delta == 0) {
        return false;
    }
    return move_bookmark_to(id, delta < 0 ? *at - 1 : *at + 1);
}

bool PlacesModel::move_bookmark_to(places::BookmarkId id, std::size_t index) {
    const std::optional<std::size_t> at = bookmarks_.index_of(id);
    if (!at || *at == std::min(index, bookmarks_.size() - 1)) {
        return false;
    }
    if (!bookmarks_.move(id, index)) {
        return false;
    }
    save_bookmarks();
    return true;
}

void PlacesModel::record_visit(const Path& folder, Timestamp now, double now_ms) {
    const Path p = path_input::normalize_absolute(folder);
    if (recents_.record(p, places::identity_for(p, volumes_), now)) {
        mark_recents(now_ms);
        missing_.erase(p); // it was just listed
    }
}

void PlacesModel::clear_recents(double now_ms) {
    if (recents_.empty()) {
        return;
    }
    recents_.clear();
    mark_recents(now_ms);
}

bool PlacesModel::toggle_tracking(double now_ms) {
    places::RecentsOptions o = recents_.options();
    o.enabled = !o.enabled;
    recents_.set_options(o);
    mark_recents(now_ms);
    return o.enabled;
}

// ---------------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------------

void PlacesModel::tick(double now_ms) {
    if (!recents_save_.due(now_ms)) {
        return;
    }
    recents_save_.taken();
    if (!recents_writer_) {
        return;
    }
    recents_writer_->submit([file = places::recents_file(options_.config_root), copy = recents_,
                             log = options_.log]() -> Result<void> {
        Result<void> r = places::save_recents(file, copy);
        if (!r && log) {
            log("recents save failed: " + r.error().message);
        }
        return r;
    });
}

void PlacesModel::final_save() {
    if (!bookmarks_writer_ || !recents_writer_) {
        return;
    }
    (void)bookmarks_writer_->flush();
    (void)recents_writer_->flush();
    // Only files whose store changed this run: an untouched store never creates a file.
    if (bookmarks_dirty_) {
        if (Result<void> r =
                places::save_bookmarks(places::bookmarks_file(options_.config_root), bookmarks_);
            !r) {
            log("bookmarks save failed: " + r.error().message);
        }
    }
    if (recents_dirty_) {
        if (Result<void> r =
                places::save_recents(places::recents_file(options_.config_root), recents_);
            !r) {
            log("recents save failed: " + r.error().message);
        }
    }
    recents_save_.taken();
}

std::uint64_t PlacesModel::writes() const {
    std::uint64_t n = 0;
    if (bookmarks_writer_) {
        n += bookmarks_writer_->writes();
    }
    if (recents_writer_) {
        n += recents_writer_->writes();
    }
    return n;
}

} // namespace navigator::ui
