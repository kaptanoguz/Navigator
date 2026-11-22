// src/search/index.cpp: the default IIndex (navigator/index.hpp): one service thread that loads,
// scans, verifies, watches and saves; queries from any thread under a shared lock.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/walk.hpp>
#include <navigator/watch.hpp>

#include "index_tree.hpp"
#include "match_core.hpp"
#include "rw_lock.hpp"

namespace navigator::search {

namespace {

using detail::Child;
using detail::Dir;
using detail::kNoIndex;
using detail::Tree;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kCoarseGuardNs = 2'000'000'000;
constexpr std::chrono::milliseconds kProgressInterval{100};
constexpr std::uint64_t kProgressEntries = 4096;
constexpr std::chrono::milliseconds kFakeWaitSlice{10};
constexpr std::size_t kDrainEveryDirs = 64;

[[nodiscard]] std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool directory_like(const Child& c) noexcept {
    return c.kind == fs::EntryKind::Directory ||
           (c.kind == fs::EntryKind::Symlink && (c.flags & detail::kEntryLinkToDir) != 0);
}

[[nodiscard]] bool valid_name(std::string_view name) noexcept {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of(std::string_view("/\0", 2)) == std::string_view::npos &&
           name.size() <= detail::kMaxIndexedName;
}

void lower_priority() noexcept {
    const auto tid = static_cast<id_t>(::syscall(SYS_gettid));
    (void)::setpriority(PRIO_PROCESS, tid, 10);
#ifdef SYS_ioprio_set
    // IOPRIO_WHO_PROCESS (1) for the calling thread, class best-effort (2), lowest level (7).
    constexpr int kWhoProcess = 1;
    constexpr int kClassShift = 13;
    (void)::syscall(SYS_ioprio_set, kWhoProcess, 0, (2 << kClassShift) | 7);
#endif
}

// One statx per directory is the whole cost of verifying an unchanged directory; fs::stat()
// would also read credentials for fields the index never uses. DONT_SYNC and NO_AUTOMOUNT keep
// a check of a mount point from waiting on a network server or triggering an automount.
struct DirStat {
    int error = 0;
    bool directory = false;
    std::int64_t mtime_ns = 0;
    FileId id;
};

[[nodiscard]] DirStat stat_dir(const std::string& path, bool follow) noexcept {
    struct statx st {};
    DirStat out;
    const int flags = AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC | (follow ? 0 : AT_SYMLINK_NOFOLLOW);
    if (::statx(AT_FDCWD, path.c_str(), flags, STATX_TYPE | STATX_INO | STATX_MTIME, &st) != 0) {
        out.error = errno;
        return out;
    }
    out.directory = S_ISDIR(st.stx_mode);
    out.mtime_ns = (static_cast<std::int64_t>(st.stx_mtime.tv_sec) * 1'000'000'000) +
                   static_cast<std::int64_t>(st.stx_mtime.tv_nsec);
    out.id = FileId{static_cast<std::uint64_t>(makedev(st.stx_dev_major, st.stx_dev_minor)),
                    static_cast<std::uint64_t>(st.stx_ino)};
    return out;
}

[[nodiscard]] std::string join(const std::string& dir, std::string_view name) {
    std::string out;
    out.reserve(dir.size() + 1 + name.size());
    out.append(dir);
    if (out.empty() || out.back() != '/') {
        out.push_back('/');
    }
    out.append(name);
    return out;
}

enum class Outcome : std::uint8_t { Ok, Cancelled, RootFailed };
enum class LoopExit : std::uint8_t { Stop, Rebuild, Failed };

class NameIndex final : public IIndex {
public:
    explicit NameIndex(NameIndexOptions options) : options_(std::move(options)) {
        tree_.reset();
        if (!options_.file.empty()) {
            own_dir_ = options_.file.parent_path().lexically_normal().native();
            own_dir_name_ = options_.file.parent_path().filename().native();
        }
        wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    }
    NameIndex(const NameIndex&) = delete;
    NameIndex& operator=(const NameIndex&) = delete;
    NameIndex(NameIndex&&) = delete;
    NameIndex& operator=(NameIndex&&) = delete;
    ~NameIndex() override {
        stop();
        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
        }
    }

    void start() override {
        const std::lock_guard lock(control_mu_);
        if (started_) {
            return;
        }
        try {
            thread_ = std::jthread([this](const std::stop_token& st) { run(st); });
            started_ = true;
        } catch (const std::exception& e) {
            const std::lock_guard sl(status_mu_);
            status_.state = IndexState::Failed;
            status_.last_error =
                Error{ErrorCode::PlatformError, std::string("index thread: ") + e.what()};
        }
    }

    void stop() noexcept override {
        const std::lock_guard lock(control_mu_);
        if (!started_ || stopped_) {
            return;
        }
        stopped_ = true;
        thread_.request_stop();
        wake();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void request_rescan(RescanKind kind) override {
        {
            const std::lock_guard lock(control_mu_);
            if (!started_ || stopped_) {
                return;
            }
        }
        {
            const std::lock_guard lock(request_mu_);
            if (!pending_ || kind == RescanKind::Rebuild) {
                pending_ = kind;
            }
        }
        wake();
    }

    [[nodiscard]] IndexStatus status() const override {
        const std::lock_guard lock(status_mu_);
        return status_;
    }

    [[nodiscard]] const Path& root() const noexcept override { return options_.root; }

    [[nodiscard]] Result<IndexResult> query(const IndexQuery& query,
                                            std::stop_token stop) const override;

private:
    // ---- service thread -----------------------------------------------------------------
    void run(const std::stop_token& st);
    void run_inner(const std::stop_token& st);
    bool load_file();
    [[nodiscard]] Outcome pass(const std::stop_token& st, bool full);
    [[nodiscard]] Outcome walk_from(std::uint32_t start, bool force, std::int64_t prev_scan_start,
                                    const std::stop_token& st, bool drain);
    [[nodiscard]] Outcome refresh_dir(std::uint32_t d, const std::string& path, bool force,
                                      std::int64_t prev_scan_start, const std::stop_token& st);
    void apply_listing(std::uint32_t d, std::vector<fs::DirEntry>& listed, std::int64_t mtime,
                       FileId id);
    void mark_unreadable(std::uint32_t d);
    void mark_other_fs(std::uint32_t d, FileId id);
    // The path the tree gives `d` still names that directory on disk.
    [[nodiscard]] bool still_at(std::uint32_t d) const;
    // `d` is missing at its path. Removes it, unless an ancestor moved and only the path is
    // stale: then the ancestor's own event or re-listing settles the subtree.
    void on_dir_missing(std::uint32_t d);
    // Directory `id` was found as `name` in `d`: forgets the record of the place it moved from.
    void release_stale_owner(FileId id, std::uint32_t d, std::string_view name);
    [[nodiscard]] bool is_own_dir(std::uint32_t d) const;
    void remove_dir_from_parent(std::uint32_t d);
    void remove_child(std::uint32_t d, std::uint32_t slot);
    void ensure_watch(std::uint32_t d, const std::string& path);
    void release_watches(const std::vector<WatchHandle>& handles);
    void drain_events(const std::stop_token& st);
    void handle_event(const WatchEvent& ev, const std::stop_token& st);
    void on_appeared(std::uint32_t d, std::string_view name, const std::stop_token& st);
    void on_vanished(std::uint32_t d, std::string_view name);
    void on_renamed(std::uint32_t d, std::string_view old_name, std::string_view new_name,
                    const std::stop_token& st);
    void on_dir_gone(std::uint32_t d, const std::stop_token& st);
    [[nodiscard]] LoopExit ready_loop(const std::stop_token& st);
    // Waits for watcher events, a request or stop, at most `timeout` (negative: unlimited).
    void wait_for_activity(const std::stop_token& st, std::chrono::milliseconds timeout);
    [[nodiscard]] std::optional<RescanKind> take_request();
    void save_now();
    [[nodiscard]] std::chrono::seconds save_delay() const {
        // A failing save is retried, but never in a busy loop.
        return save_failed_ ? std::max(options_.save_interval, std::chrono::seconds{1})
                            : options_.save_interval;
    }
    void clear_all();

    // Exclusive section: mutates tree_, bumps the generation, marks dirty.
    template <class F> void mutate(F&& f) {
        {
            const std::unique_lock lock(data_mu_);
            f();
            const std::lock_guard sl(status_mu_);
            ++status_.generation;
        }
        dirty_ = true;
    }

    void set_state(IndexState state);
    void set_error(Error error);
    // Refreshes the counters; notifies when `force` or the progress interval has passed.
    void publish(bool force);
    void notify() const {
        if (options_.on_status_change) {
            options_.on_status_change();
        }
    }
    void wake() const noexcept {
        if (wake_fd_ >= 0) {
            const std::uint64_t one = 1;
            (void)!::write(wake_fd_, &one, sizeof(one));
        }
    }

    NameIndexOptions options_;

    mutable detail::WriterPreferringMutex data_mu_;
    Tree tree_; // written by the service thread under data_mu_ (exclusive)

    mutable std::mutex status_mu_;
    IndexStatus status_;

    std::mutex control_mu_;
    bool started_ = false;
    bool stopped_ = false;
    std::jthread thread_;

    std::mutex request_mu_;
    std::optional<RescanKind> pending_;
    int wake_fd_ = -1;

    // Service thread only.
    std::unique_ptr<IWatcher> watcher_;
    std::unordered_map<WatchHandle, std::uint32_t> watched_;
    std::vector<WatchEvent> events_;
    bool exhausted_ = false;
    bool verify_requested_ = false;
    bool root_failed_ = false;
    bool save_failed_ = false;
    std::uint64_t rename_epoch_ = 0; // bumped when a directory is renamed (queued paths go stale)
    std::string own_dir_;            // the index file's directory: its changes never trigger a save
    std::string own_dir_name_;
    bool dirty_ = false;
    std::uint64_t root_device_ = 0;
    std::size_t dirs_pending_ = 0;
    Clock::time_point last_save_ = Clock::now();
    Clock::time_point last_publish_{};
    std::uint64_t published_entries_ = 0;
    std::string fold_scratch_;
};

// ---------------------------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------------------------

void NameIndex::set_state(IndexState state) {
    {
        const std::lock_guard lock(status_mu_);
        if ((status_.state == IndexState::Ready) != (state == IndexState::Ready)) {
            ++status_.generation; // IndexResult::complete differs
        }
        status_.state = state;
    }
    publish(true);
}

void NameIndex::set_error(Error error) {
    {
        const std::lock_guard lock(status_mu_);
        status_.last_error = std::move(error);
    }
    publish(true);
}

void NameIndex::publish(bool force) {
    const Clock::time_point now = Clock::now();
    if (!force && now - last_publish_ < kProgressInterval &&
        tree_.entries < published_entries_ + kProgressEntries) {
        return;
    }
    last_publish_ = now;
    published_entries_ = tree_.entries;
    std::uint64_t unreadable = 0;
    std::uint64_t listable = 0;
    for (const Dir& d : tree_.dirs) {
        if (!d.alive) {
            continue;
        }
        if ((d.flags & detail::kDirUnreadable) != 0) {
            ++unreadable;
        }
        if ((d.flags & detail::kDirOtherFs) == 0) {
            ++listable;
        }
    }
    {
        const std::lock_guard lock(status_mu_);
        status_.entries = tree_.entries;
        status_.directories = tree_.live_dirs;
        status_.dirs_pending = dirs_pending_;
        status_.dirs_unwatched = listable > watched_.size() ? listable - watched_.size() : 0;
        status_.skipped_permission = unreadable;
        status_.live = watcher_ != nullptr && !watched_.empty();
        status_.watch_budget_exhausted = exhausted_;
        status_.truncated = tree_.truncated;
    }
    notify();
}

// ---------------------------------------------------------------------------------------------
// Service thread
// ---------------------------------------------------------------------------------------------

void NameIndex::run(const std::stop_token& st) {
    bool crashed = true;
    try {
        run_inner(st);
        crashed = false;
    } catch (const std::bad_alloc&) {
        set_error(Error{ErrorCode::OutOfMemory, "index: out of memory"});
    } catch (const std::exception& e) {
        set_error(Error{ErrorCode::Unknown, std::string("index: ") + e.what()});
    }
    if (crashed) {
        watcher_.reset();
        set_state(IndexState::Failed); // Stopped only once stop() is called
        while (!st.stop_requested()) {
            wait_for_activity(st, std::chrono::milliseconds{-1});
        }
    }
    try {
        if (dirty_ && !options_.file.empty()) {
            save_now();
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): best-effort final save
    }
    watched_.clear();
    watcher_.reset();
    set_state(IndexState::Stopped);
}

bool NameIndex::load_file() {
    if (options_.file.empty()) {
        return false;
    }
    const Clock::time_point t0 = Clock::now();
    Result<std::string> bytes = detail::read_file(options_.file);
    if (!bytes) {
        if (bytes.error().code != ErrorCode::NotFound) {
            if (bytes.error().code == ErrorCode::InvalidArgument) { // corrupt, not just unreadable
                std::error_code ec;
                std::filesystem::remove(options_.file, ec);
            }
            set_error(bytes.error());
        }
        return false;
    }
    Result<Tree> tree = fail(ErrorCode::OutOfMemory, "index file too large to load");
    try {
        tree = detail::decode_index(*bytes, options_.identity);
    } catch (const std::bad_alloc&) {
        tree = fail(ErrorCode::OutOfMemory); // no allocation here; rebuilt from scratch
    }
    bytes->clear();
    bytes->shrink_to_fit();
    if (!tree) {
        std::error_code ec;
        std::filesystem::remove(options_.file, ec);
        set_error(tree.error());
        return false;
    }
    {
        const std::unique_lock lock(data_mu_);
        tree_ = std::move(*tree);
        const std::lock_guard sl(status_mu_);
        ++status_.generation;
        status_.loaded_from_file = true;
        status_.load_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    }
    return true;
}

void NameIndex::run_inner(const std::stop_token& st) {
    if (options_.background_priority) {
        lower_priority();
    }
    set_state(IndexState::Loading);
    bool have_data = load_file();
    if (options_.watcher_factory && options_.watch_budget > 0) {
        Result<std::unique_ptr<IWatcher>> w = options_.watcher_factory();
        if (w && *w) {
            watcher_ = std::move(*w);
        } else if (!w) {
            set_error(w.error());
        }
    }
    bool rebuild = false;
    while (!st.stop_requested()) {
        if (rebuild) {
            // pass() clears the tree once the root is known to be readable: until then queries
            // keep serving what there is.
            have_data = false;
            rebuild = false;
        }
        set_state(have_data && tree_.complete ? IndexState::Verifying : IndexState::Scanning);
        const bool scanning = !have_data || !tree_.complete;
        const Outcome r = pass(st, !have_data);
        if (r == Outcome::Cancelled) {
            return;
        }
        LoopExit exit = LoopExit::Failed;
        if (r == Outcome::Ok) {
            have_data = true;
            set_state(IndexState::Ready);
            // A scan is expensive: save it at once. What a verify pass found waits for the
            // debounced save.
            if (scanning && dirty_ && !options_.file.empty()) {
                save_now();
            }
            exit = ready_loop(st);
        }
        if (exit == LoopExit::Stop) {
            return;
        }
        if (exit == LoopExit::Rebuild) {
            rebuild = true;
            continue;
        }
        // Failed: wait for the next rescan interval or an explicit request, then start over.
        set_state(IndexState::Failed);
        const auto interval =
            std::chrono::duration_cast<std::chrono::milliseconds>(options_.rescan_interval);
        const Clock::time_point until = Clock::now() + interval;
        while (!st.stop_requested()) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
            if (options_.rescan_interval.count() > 0 && left.count() <= 0) {
                break;
            }
            wait_for_activity(
                st, options_.rescan_interval.count() > 0 ? left : std::chrono::milliseconds{-1});
            if (watcher_) {
                events_.clear();
                (void)watcher_->poll(events_); // discard: everything is re-read anyway
            }
            if (take_request()) {
                break;
            }
        }
        root_failed_ = false;
        rebuild = true;
    }
}

void NameIndex::clear_all() {
    std::vector<WatchHandle> handles;
    for (const Dir& d : tree_.dirs) {
        if (d.alive && d.watch != kInvalidWatchHandle) {
            handles.push_back(d.watch);
        }
    }
    mutate([&] { tree_.reset(); });
    release_watches(handles);
    watched_.clear();
    exhausted_ = false;
}

Outcome NameIndex::pass(const std::stop_token& st, bool full) {
    const Clock::time_point t0 = Clock::now();
    const std::int64_t pass_start = now_ns();
    const std::int64_t prev_scan_start = tree_.scan_started_at;
    Result<fs::DirEntry> root = fs::stat(options_.root, true);
    if (!root || !fs::is_directory_like(*root)) {
        set_error(root ? Error{ErrorCode::InvalidArgument,
                               "index root is not a directory: " + options_.root.native()}
                       : root.error());
        return Outcome::RootFailed;
    }
    root_device_ = root->id.device;
    const Dir& r0 = tree_.dirs[0];
    if (!full && (r0.device != 0 || r0.inode != 0) &&
        (r0.device != root->id.device || r0.inode != root->id.inode)) {
        full = true; // another filesystem is mounted there now
    }
    if (full) {
        clear_all();
    }
    if (full || !tree_.complete) {
        mutate([&] { tree_.complete = false; });
    }
    const Outcome o = walk_from(0, full, prev_scan_start, st, true);
    dirs_pending_ = 0;
    if (o != Outcome::Ok) {
        publish(true);
        return o;
    }
    tree_.scan_started_at = pass_start; // service-thread only; saved with the next change
    if (!tree_.complete) {
        mutate([&] { tree_.complete = true; });
    }
    {
        const std::lock_guard lock(status_mu_);
        status_.last_complete_scan = Timestamp{std::chrono::nanoseconds{now_ns()}};
        status_.scan_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    }
    publish(true);
    return Outcome::Ok;
}

Outcome NameIndex::walk_from(std::uint32_t start, bool force, std::int64_t prev_scan_start,
                             const std::stop_token& st, bool drain) {
    // Paths travel with the queue (one append per directory instead of a climb to the root).
    // Events handled meanwhile can free or reuse a queued slot (serial) or rename an ancestor
    // of a queued directory (epoch).
    struct Queued {
        std::uint32_t dir;
        std::uint64_t serial;
        std::uint64_t epoch;
        std::string path;
    };
    std::deque<Queued> queue;
    queue.push_back(Queued{start, tree_.dirs[start].serial, rename_epoch_,
                           tree_.path_of(start, options_.root).native()});
    std::size_t processed = 0;
    while (!queue.empty()) {
        if (st.stop_requested()) {
            return Outcome::Cancelled;
        }
        Queued item = std::move(queue.front());
        queue.pop_front();
        const std::uint32_t d = item.dir;
        if (d >= tree_.dirs.size() || !tree_.dirs[d].alive || tree_.dirs[d].serial != item.serial) {
            continue;
        }
        if (item.epoch != rename_epoch_) {
            item.path = tree_.path_of(d, options_.root).native();
            item.epoch = rename_epoch_;
        }
        const std::string& path = item.path;
        const Outcome o = refresh_dir(d, path, force, prev_scan_start, st);
        if (o != Outcome::Ok) {
            return o;
        }
        if (d < tree_.dirs.size() && tree_.dirs[d].alive) {
            const Dir& rec = tree_.dirs[d];
            for (const Child& c : rec.children) {
                if (c.dir != kNoIndex) {
                    queue.push_back(Queued{c.dir, tree_.dirs[c.dir].serial, item.epoch,
                                           join(path, Tree::raw_name(rec, c))});
                }
            }
        }
        if (drain) {
            dirs_pending_ = queue.size();
        }
        if (++processed % kDrainEveryDirs == 0) {
            if (drain) {
                drain_events(st);
                if (root_failed_) {
                    return Outcome::RootFailed;
                }
                if (dirty_ && !options_.file.empty() && Clock::now() - last_save_ >= save_delay()) {
                    save_now();
                }
            }
            publish(false);
        }
    }
    return Outcome::Ok;
}

Outcome NameIndex::refresh_dir(std::uint32_t d, const std::string& path, bool force,
                               std::int64_t prev_scan_start, const std::stop_token& st) {
    const bool mount = (tree_.dirs[d].flags & detail::kDirOtherFs) != 0;
    if (!mount) {
        ensure_watch(d, path); // before the stat and the listing, so no change falls between
    }
    const DirStat s = stat_dir(path, d == 0);
    if (s.error != 0) {
        if (d == 0) {
            set_error(Error{ErrorCode::NotFound,
                            "index root " + path + ": " + std::system_category().message(s.error)});
            return Outcome::RootFailed;
        }
        if (s.error == ENOENT || s.error == ENOTDIR) {
            on_dir_missing(d);
        }
        return Outcome::Ok;
    }
    if (!s.directory) {
        if (d == 0) {
            return Outcome::RootFailed;
        }
        on_dir_missing(d);
        return Outcome::Ok;
    }
    if (d != 0 && s.id.device != root_device_) {
        mark_other_fs(d, s.id); // one volume: something is mounted here
        return Outcome::Ok;
    }
    if (mount) { // unmounted since: an ordinary directory of this volume again
        mutate([&] { tree_.dirs[d].flags &= ~(detail::kDirOtherFs | detail::kDirScanned); });
        ensure_watch(d, path);
    }
    const Dir& rec = tree_.dirs[d];
    const std::int64_t mtime = s.mtime_ns;
    const bool same_inode = rec.device == s.id.device && rec.inode == s.id.inode;
    const bool need = force || (rec.flags & detail::kDirScanned) == 0 ||
                      (rec.flags & detail::kDirUnreadable) != 0 || !same_inode ||
                      mtime != rec.mtime_ns || rec.mtime_ns >= prev_scan_start - kCoarseGuardNs;
    if (!need) {
        return Outcome::Ok;
    }
    fs::WalkOptions wo;
    wo.root = path;
    wo.max_depth = 1;
    wo.follow_symlinks = false;
    wo.cross_filesystems = false;
    wo.show_hidden = true;
    wo.skip_unreadable = true;
    wo.metadata = false;
    wo.batch_size = 4096;
    wo.batch_latency = std::chrono::milliseconds{1000};
    wo.max_entries = 0;
    Result<fs::Walker> walker = fs::Walker::open(wo);
    if (!walker) {
        const ErrorCode code = walker.error().code;
        if (code == ErrorCode::PermissionDenied) {
            mark_unreadable(d);
        } else if (code == ErrorCode::NotFound || code == ErrorCode::InvalidArgument) {
            if (d == 0) {
                return Outcome::RootFailed;
            }
            on_dir_missing(d);
        }
        return Outcome::Ok;
    }
    std::vector<fs::DirEntry> listed;
    fs::WalkBatch batch;
    while (true) {
        Result<bool> more = walker->next_batch(batch, st);
        if (!more) {
            if (more.error().code == ErrorCode::Cancelled || st.stop_requested()) {
                return Outcome::Cancelled;
            }
            if (more.error().code == ErrorCode::PermissionDenied) {
                mark_unreadable(d);
            }
            return Outcome::Ok;
        }
        for (fs::WalkEntry& e : batch.entries) {
            listed.push_back(std::move(e.entry));
        }
        if (!*more) {
            break;
        }
    }
    // The walker opens the directory a second time and only reports, as a skip, a failure there
    // (no search permission, read error): that listing is empty or partial, not the truth.
    if (walker->stats().skipped_permission > 0) {
        mark_unreadable(d);
        return Outcome::Ok;
    }
    if (walker->stats().skipped_vanished > 0 || walker->stats().skipped_errors > 0) {
        return Outcome::Ok;
    }
    const DirStat after = stat_dir(path, d == 0);
    if (after.error != 0 || !after.directory || after.id != s.id) {
        return Outcome::Ok; // replaced (possibly by a symlink) while it was listed
    }
    apply_listing(d, listed, mtime, s.id);
    return Outcome::Ok;
}

void NameIndex::mark_unreadable(std::uint32_t d) {
    if ((tree_.dirs[d].flags & detail::kDirUnreadable) != 0 && tree_.dirs[d].children.empty()) {
        return; // retried by every pass: nothing changed, nothing to save
    }
    std::vector<WatchHandle> handles;
    mutate([&] {
        Dir& rec = tree_.dirs[d];
        for (const Child& c : rec.children) {
            if (c.dir != kNoIndex) {
                tree_.free_subtree(c.dir, handles);
            }
        }
        tree_.entries -= rec.children.size();
        rec.children.clear();
        rec.pool.clear();
        rec.garbage = 0;
        rec.flags |= detail::kDirUnreadable | detail::kDirScanned;
    });
    release_watches(handles);
}

void NameIndex::mark_other_fs(std::uint32_t d, FileId id) {
    const Dir& cur = tree_.dirs[d];
    if ((cur.flags & detail::kDirOtherFs) != 0 && cur.children.empty() && cur.device == id.device &&
        cur.inode == id.inode) {
        return;
    }
    std::vector<WatchHandle> handles;
    mutate([&] {
        Dir& rec = tree_.dirs[d];
        for (const Child& c : rec.children) {
            if (c.dir != kNoIndex) {
                tree_.free_subtree(c.dir, handles);
            }
        }
        tree_.entries -= rec.children.size();
        rec.children.clear();
        rec.pool.clear();
        rec.garbage = 0;
        rec.mtime_ns = 0;
        rec.flags = detail::kDirOtherFs;
        tree_.set_identity(d, id.device, id.inode);
    });
    Dir& rec = tree_.dirs[d];
    if (rec.watch != kInvalidWatchHandle) {
        handles.push_back(rec.watch);
        rec.watch = kInvalidWatchHandle;
    }
    release_watches(handles);
}

bool NameIndex::still_at(std::uint32_t d) const {
    const Dir& rec = tree_.dirs[d];
    const DirStat s = stat_dir(tree_.path_of(d, options_.root).native(), d == 0);
    return s.error == 0 && s.directory && s.id.device == rec.device && s.id.inode == rec.inode;
}

void NameIndex::on_dir_missing(std::uint32_t d) {
    const std::uint32_t parent = tree_.dirs[d].parent;
    if (parent != kNoIndex && still_at(parent)) {
        remove_dir_from_parent(d);
    }
}

bool NameIndex::is_own_dir(std::uint32_t d) const {
    return !own_dir_.empty() && tree_.dirs[d].name == own_dir_name_ &&
           tree_.path_of(d, options_.root).native() == own_dir_;
}

void NameIndex::release_stale_owner(FileId id, std::uint32_t d, std::string_view name) {
    if (id.device == 0 && id.inode == 0) {
        return;
    }
    const auto it = tree_.by_id.find(id);
    if (it == tree_.by_id.end()) {
        return;
    }
    const std::uint32_t owner = it->second;
    const Dir& rec = tree_.dirs[owner];
    if (owner == 0 || !rec.alive || (rec.parent == d && rec.name == name)) {
        return; // the root, or this very child
    }
    if (still_at(owner)) {
        return; // really reachable twice (bind mount): entered once
    }
    for (std::uint32_t up = d; up != kNoIndex; up = tree_.dirs[up].parent) {
        if (up == owner) {
            // d's own path is stale: freeing the owner would free d. A verify pass sorts it out.
            verify_requested_ = true;
            return;
        }
    }
    const std::uint32_t old_parent = rec.parent;
    remove_dir_from_parent(owner);
    // Another directory may have taken the old name: have the old parent listed again.
    if (old_parent != kNoIndex && old_parent != d) {
        tree_.dirs[old_parent].mtime_ns = 0;
    }
}

void NameIndex::apply_listing(std::uint32_t d, std::vector<fs::DirEntry>& listed,
                              std::int64_t mtime, FileId id) {
    const bool was_dirty = dirty_;
    const bool own = is_own_dir(d);
    const Path dir_path = tree_.path_of(d, options_.root);
    for (fs::DirEntry& e : listed) {
        if (e.kind != fs::EntryKind::Directory) {
            continue;
        }
        if (e.id == FileId{}) {
            if (const Result<fs::DirEntry> cs = fs::stat(dir_path / e.name, false); cs) {
                e.id = cs->id;
            }
        }
        // First, because it removes records (possibly children of d): moved directories.
        release_stale_owner(e.id, d, e.name);
    }

    const Dir& old = tree_.dirs[d];
    const std::size_t old_size = old.children.size();
    std::unordered_map<std::string_view, std::uint32_t> old_by_name;
    old_by_name.reserve(old_size);
    for (std::uint32_t i = 0; i < old_size; ++i) {
        old_by_name.emplace(Tree::raw_name(old, old.children[i]), i);
    }
    std::vector<bool> kept(old_size, false);
    std::vector<bool> relinked(old_size, false);
    std::vector<Child> children;
    children.reserve(listed.size());
    std::string pool;
    struct NewDir {
        std::size_t child;
        FileId id;
    };
    std::vector<NewDir> new_dirs;

    const auto add = [&](const fs::DirEntry& e, std::uint32_t old_slot) {
        Child c;
        if (!detail::append_name(pool, c, e.name, fold_scratch_)) {
            return;
        }
        c.kind = e.kind;
        c.flags = static_cast<std::uint8_t>(
            (e.hidden || fs::is_hidden_name(e.name) ? detail::kEntryHidden : 0U) |
            (e.kind == fs::EntryKind::Symlink && e.target_kind == fs::EntryKind::Directory
                 ? detail::kEntryLinkToDir
                 : 0U));
        if (e.kind == fs::EntryKind::Directory) {
            if (old_slot != kNoIndex) {
                const std::uint32_t prev = old.children[old_slot].dir;
                // Same name is not enough: the directory may have been replaced by another.
                if (prev != kNoIndex &&
                    (e.id == FileId{} ||
                     e.id == FileId{tree_.dirs[prev].device, tree_.dirs[prev].inode})) {
                    c.dir = prev;
                    relinked[old_slot] = true;
                }
            }
            if (c.dir == kNoIndex) {
                new_dirs.push_back(NewDir{children.size(), e.id});
            }
        }
        children.push_back(c);
    };

    // What is already indexed stays; only new names count against max_entries.
    std::vector<const fs::DirEntry*> fresh;
    for (const fs::DirEntry& e : listed) {
        if (!valid_name(e.name)) {
            continue;
        }
        const auto it = old_by_name.find(e.name);
        if (it == old_by_name.end()) {
            fresh.push_back(&e);
        } else if (!kept[it->second]) {
            kept[it->second] = true;
            add(e, it->second);
        }
    }
    const std::uint64_t used = tree_.entries - old_size + children.size();
    const std::uint64_t room = options_.max_entries > used ? options_.max_entries - used : 0;
    const bool truncated = fresh.size() > room;
    if (truncated) {
        fresh.resize(static_cast<std::size_t>(room));
    }
    for (const fs::DirEntry* e : fresh) {
        add(*e, kNoIndex);
    }

    std::vector<WatchHandle> handles;
    mutate([&] {
        for (std::size_t i = 0; i < old_size; ++i) {
            const std::uint32_t sub = tree_.dirs[d].children[i].dir;
            if (sub != kNoIndex && !relinked[i]) {
                tree_.free_subtree(sub, handles);
            }
        }
        tree_.set_identity(d, id.device, id.inode); // before the cycle check of the children
        for (const NewDir& nd : new_dirs) {
            Child& c = children[nd.child];
            const bool known = nd.id != FileId{};
            if (known && tree_.by_id.contains(nd.id)) {
                continue; // entered elsewhere (bind mount loop): not entered again
            }
            const std::uint32_t sub =
                tree_.new_dir(d, std::string_view(pool).substr(c.raw_off, c.raw_len));
            tree_.set_identity(sub, nd.id.device, nd.id.inode);
            if (known && nd.id.device != root_device_) {
                tree_.dirs[sub].flags |= detail::kDirOtherFs;
            }
            c.dir = sub;
        }
        Dir& rec = tree_.dirs[d];
        // free_subtree() already subtracted what was below the removed children.
        tree_.entries = tree_.entries - old_size + children.size();
        rec.children = std::move(children);
        rec.pool = std::move(pool);
        rec.garbage = 0;
        rec.mtime_ns = mtime;
        rec.flags = (rec.flags | detail::kDirScanned) & ~detail::kDirUnreadable;
        if (truncated) {
            tree_.truncated = true;
        }
    });
    release_watches(handles);
    if (own) {
        dirty_ = was_dirty;
    }
}

void NameIndex::remove_child(std::uint32_t d, std::uint32_t slot) {
    std::vector<WatchHandle> handles;
    mutate([&] {
        Dir& rec = tree_.dirs[d];
        const Child c = rec.children[slot];
        if (c.dir != kNoIndex) {
            tree_.free_subtree(c.dir, handles);
        }
        rec.garbage += c.raw_len + (c.fold_off == c.raw_off ? 0U : c.fold_len);
        rec.children[slot] = rec.children.back();
        rec.children.pop_back();
        --tree_.entries;
        if (rec.garbage > 4096 && rec.garbage * 2 > rec.pool.size()) {
            detail::compact_pool(rec, fold_scratch_);
        }
    });
    release_watches(handles);
}

void NameIndex::remove_dir_from_parent(std::uint32_t d) {
    const std::uint32_t p = tree_.dirs[d].parent;
    if (p == kNoIndex) {
        return;
    }
    const std::vector<Child>& siblings = tree_.dirs[p].children;
    for (std::uint32_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].dir == d) {
            remove_child(p, i);
            return;
        }
    }
    std::vector<WatchHandle> handles;
    mutate([&] { tree_.free_subtree(d, handles); });
    release_watches(handles);
}

void NameIndex::ensure_watch(std::uint32_t d, const std::string& path) {
    if (!watcher_ || exhausted_ || tree_.dirs[d].watch != kInvalidWatchHandle ||
        watched_.size() >= options_.watch_budget) {
        return;
    }
    Result<WatchHandle> h = watcher_->add(Path(path));
    if (!h) {
        if (h.error().code == ErrorCode::PlatformError) {
            exhausted_ = true;
            set_error(h.error());
        }
        return;
    }
    if (watched_.contains(*h)) {
        watcher_->remove(*h); // same inode watched twice: keep the first owner
        return;
    }
    tree_.dirs[d].watch = *h; // service-thread bookkeeping; queries never read it
    watched_.emplace(*h, d);
}

void NameIndex::release_watches(const std::vector<WatchHandle>& handles) {
    for (const WatchHandle h : handles) {
        if (watched_.erase(h) > 0 && watcher_) {
            watcher_->remove(h);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Live updates
// ---------------------------------------------------------------------------------------------

void NameIndex::drain_events(const std::stop_token& st) {
    if (!watcher_) {
        return;
    }
    // Several rounds: handling an event may add watches whose events arrive meanwhile.
    for (int round = 0; round < 4; ++round) {
        events_.clear();
        Result<std::size_t> n = watcher_->poll(events_);
        if (!n) {
            verify_requested_ = true;
            return;
        }
        if (*n == 0) {
            break;
        }
        std::vector<WatchEvent> batch;
        batch.swap(events_);
        for (const WatchEvent& ev : batch) {
            if (st.stop_requested()) {
                return;
            }
            handle_event(ev, st);
        }
        publish(true);
    }
}

void NameIndex::handle_event(const WatchEvent& ev, const std::stop_token& st) {
    if (ev.kind == WatchEventKind::Overflow) {
        verify_requested_ = true;
        return;
    }
    const auto it = watched_.find(ev.handle);
    if (it == watched_.end()) {
        return;
    }
    const std::uint32_t d = it->second;
    if (d >= tree_.dirs.size() || !tree_.dirs[d].alive) {
        return;
    }
    const bool was_dirty = dirty_;
    const bool own = is_own_dir(d);
    switch (ev.kind) {
    case WatchEventKind::Created:
    case WatchEventKind::MovedTo: on_appeared(d, ev.name, st); break;
    case WatchEventKind::Deleted:
    case WatchEventKind::MovedFrom: on_vanished(d, ev.name); break;
    case WatchEventKind::Renamed: on_renamed(d, ev.old_name, ev.name, st); break;
    case WatchEventKind::WatchedDirGone:
        watched_.erase(it);
        tree_.dirs[d].watch = kInvalidWatchHandle;
        on_dir_gone(d, st);
        break;
    case WatchEventKind::Modified:
    case WatchEventKind::Attrib:
    case WatchEventKind::Overflow: break;
    }
    if (own) {
        dirty_ = was_dirty; // the index's own saves would otherwise cause the next save
    }
}

void NameIndex::on_appeared(std::uint32_t d, std::string_view name, const std::stop_token& st) {
    if (!valid_name(name) || (tree_.dirs[d].flags & detail::kDirScanned) == 0) {
        return; // a directory that was never listed gets all its names when it is
    }
    const Path path = tree_.path_of(d, options_.root) / std::string(name);
    Result<fs::DirEntry> s = fs::stat(path, false);
    if (!s) {
        // When d's own path is stale (an ancestor moved) the rename event re-checks the subtree.
        if (s.error().code == ErrorCode::NotFound && still_at(d)) {
            on_vanished(d, name);
        }
        return;
    }
    const std::uint32_t existing = tree_.find_child(d, name);
    if (existing != kNoIndex) {
        const Child& c = tree_.dirs[d].children[existing];
        const bool is_dir = s->kind == fs::EntryKind::Directory;
        if (c.kind == s->kind && (!is_dir || c.dir == kNoIndex ||
                                  (tree_.dirs[c.dir].device == s->id.device &&
                                   tree_.dirs[c.dir].inode == s->id.inode))) {
            if (is_dir && c.dir != kNoIndex) {
                // Same directory: refresh it (it may have been replaced by a move).
                (void)walk_from(c.dir, false, tree_.scan_started_at, st, false);
            }
            return;
        }
        remove_child(d, existing);
    }
    if (s->kind == fs::EntryKind::Directory) {
        const std::uint64_t serial = tree_.dirs[d].serial;
        release_stale_owner(s->id, d, name);
        if (!tree_.dirs[d].alive || tree_.dirs[d].serial != serial) {
            return;
        }
    }
    if (tree_.entries >= options_.max_entries) {
        mutate([&] { tree_.truncated = true; });
        return;
    }
    std::uint32_t new_dir = kNoIndex;
    bool enter = false;
    mutate([&] {
        Dir& rec = tree_.dirs[d];
        Child c;
        if (!detail::append_name(rec.pool, c, name, fold_scratch_)) {
            return;
        }
        c.kind = s->kind;
        c.flags = static_cast<std::uint8_t>(
            (fs::is_hidden_name(name) ? detail::kEntryHidden : 0U) |
            (s->kind == fs::EntryKind::Symlink && s->target_kind == fs::EntryKind::Directory
                 ? detail::kEntryLinkToDir
                 : 0U));
        if (s->kind == fs::EntryKind::Directory) {
            const auto it = tree_.by_id.find(s->id);
            const bool seen = it != tree_.by_id.end() && it->second < tree_.dirs.size() &&
                              tree_.dirs[it->second].alive;
            if (!seen) {
                new_dir = tree_.new_dir(d, name);
                tree_.set_identity(new_dir, s->id.device, s->id.inode);
                if (s->id.device != root_device_) {
                    tree_.dirs[new_dir].flags |= detail::kDirOtherFs;
                } else {
                    enter = true;
                }
                c.dir = new_dir;
            }
        }
        tree_.dirs[d].children.push_back(c);
        ++tree_.entries;
    });
    if (enter) {
        (void)walk_from(new_dir, true, tree_.scan_started_at, st, false);
    }
}

void NameIndex::on_vanished(std::uint32_t d, std::string_view name) {
    const std::uint32_t slot = tree_.find_child(d, name);
    if (slot != kNoIndex) {
        remove_child(d, slot);
    }
}

void NameIndex::on_renamed(std::uint32_t d, std::string_view old_name, std::string_view new_name,
                           const std::stop_token& st) {
    if (old_name == new_name || !valid_name(new_name)) {
        return;
    }
    if (tree_.find_child(d, old_name) == kNoIndex) {
        on_appeared(d, new_name, st);
        return;
    }
    on_vanished(d, new_name); // renamed over an existing name
    std::uint32_t renamed_dir = kNoIndex;
    const std::uint32_t slot = tree_.find_child(d, old_name);
    if (slot == kNoIndex) {
        return;
    }
    mutate([&] {
        Dir& rec = tree_.dirs[d];
        Child& c = rec.children[slot];
        const std::size_t old_bytes = c.raw_len + (c.fold_off == c.raw_off ? 0U : c.fold_len);
        Child renamed = c;
        if (!detail::append_name(rec.pool, renamed, new_name, fold_scratch_)) {
            return;
        }
        renamed.flags = static_cast<std::uint8_t>(
            (static_cast<unsigned>(renamed.flags) & ~unsigned{detail::kEntryHidden}) |
            (fs::is_hidden_name(new_name) ? detail::kEntryHidden : 0U));
        rec.children[slot] = renamed;
        rec.garbage += old_bytes;
        if (renamed.dir != kNoIndex) {
            tree_.dirs[renamed.dir].name.assign(new_name);
            renamed_dir = renamed.dir;
        }
        Dir& again = tree_.dirs[d];
        if (again.garbage > 4096 && again.garbage * 2 > again.pool.size()) {
            detail::compact_pool(again, fold_scratch_);
        }
    });
    if (renamed_dir != kNoIndex) {
        ++rename_epoch_;
        // Events that arrived before this one were resolved against the old path: re-check.
        (void)walk_from(renamed_dir, false, tree_.scan_started_at, st, false);
    }
}

void NameIndex::on_dir_gone(std::uint32_t d, const std::stop_token& st) {
    if (!still_at(d)) {
        if (d == 0) {
            root_failed_ = true;
            set_error(Error{ErrorCode::NotFound, "index root is gone: " + options_.root.native()});
            return;
        }
        on_dir_missing(d);
        return;
    }
    // Still there (renamed in place, or the kernel dropped the watch): watch and re-read.
    (void)walk_from(d, false, tree_.scan_started_at, st, false);
}

// ---------------------------------------------------------------------------------------------
// Ready loop
// ---------------------------------------------------------------------------------------------

std::optional<RescanKind> NameIndex::take_request() {
    const std::lock_guard lock(request_mu_);
    std::optional<RescanKind> r = pending_;
    pending_.reset();
    return r;
}

void NameIndex::wait_for_activity(const std::stop_token& st, std::chrono::milliseconds timeout) {
    const bool unlimited = timeout.count() < 0;
    const int fd = watcher_ ? watcher_->native_handle() : -1;
    if (watcher_ && fd < 0) {
        // Backend without a pollable descriptor (test fakes): poll in short slices.
        const Clock::time_point until = Clock::now() + timeout;
        while (!st.stop_requested()) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
            if (!unlimited && left.count() <= 0) {
                return;
            }
            const auto slice = unlimited ? kFakeWaitSlice : std::min(left, kFakeWaitSlice);
            if (watcher_->wait(slice)) {
                return;
            }
            std::uint64_t v = 0;
            if (wake_fd_ >= 0 && ::read(wake_fd_, &v, sizeof(v)) == sizeof(v)) {
                return;
            }
        }
        return;
    }
    pollfd fds[2]{};
    nfds_t n = 0;
    if (wake_fd_ >= 0) {
        fds[n++] = pollfd{wake_fd_, POLLIN, 0};
    }
    if (fd >= 0) {
        fds[n++] = pollfd{fd, POLLIN, 0};
    }
    const int ms = unlimited
                       ? -1
                       : static_cast<int>(std::min<std::chrono::milliseconds::rep>(
                             timeout.count(), std::chrono::milliseconds::rep{24} * 3600 * 1000));
    if (n == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{std::min(ms < 0 ? 50 : ms, 50)});
        return;
    }
    const int r = ::poll(fds, n, ms);
    if (r > 0 && wake_fd_ >= 0 && (fds[0].revents & POLLIN) != 0) {
        std::uint64_t v = 0;
        (void)!::read(wake_fd_, &v, sizeof(v));
    }
}

LoopExit NameIndex::ready_loop(const std::stop_token& st) {
    Clock::time_point last_verify = Clock::now();
    while (!st.stop_requested()) {
        const Clock::time_point now = Clock::now();
        std::chrono::milliseconds timeout{-1};
        const auto consider = [&](Clock::time_point deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto clamped = std::max(left, std::chrono::milliseconds{0});
            timeout = timeout.count() < 0 ? clamped : std::min(timeout, clamped);
        };
        if (options_.rescan_interval.count() > 0) {
            consider(last_verify + options_.rescan_interval);
        }
        if (dirty_ && !options_.file.empty()) {
            consider(last_save_ + save_delay());
        }
        if (verify_requested_) {
            consider(now); // an Overflow seen while the last pass ran
        }
        wait_for_activity(st, timeout);
        if (st.stop_requested()) {
            break;
        }
        if (const std::optional<RescanKind> req = take_request()) {
            if (*req == RescanKind::Rebuild) {
                return LoopExit::Rebuild;
            }
            verify_requested_ = true;
        }
        drain_events(st);
        if (root_failed_) {
            return LoopExit::Failed;
        }
        const bool periodic = options_.rescan_interval.count() > 0 &&
                              Clock::now() - last_verify >= options_.rescan_interval;
        if (verify_requested_ || periodic) {
            verify_requested_ = false;
            set_state(IndexState::Verifying);
            const Outcome o = pass(st, false);
            last_verify = Clock::now();
            if (o == Outcome::Cancelled) {
                return LoopExit::Stop;
            }
            if (o == Outcome::RootFailed) {
                return LoopExit::Failed;
            }
            set_state(IndexState::Ready);
        }
        if (dirty_ && !options_.file.empty() && Clock::now() - last_save_ >= save_delay()) {
            save_now();
        }
    }
    return LoopExit::Stop;
}

void NameIndex::save_now() {
    last_save_ = Clock::now();
    save_failed_ = true;
    Result<std::string> bytes = detail::encode_index(tree_, options_.identity, now_ns());
    if (!bytes) {
        set_error(bytes.error());
        return;
    }
    Result<void> w = detail::write_file_atomic(options_.file, *bytes);
    if (!w) {
        set_error(w.error());
        return;
    }
    dirty_ = false;
    save_failed_ = false;
}

// ---------------------------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------------------------

Result<IndexResult> NameIndex::query(const IndexQuery& q, std::stop_token stop) const {
    try {
        std::vector<std::string> comps;
        if (!q.under.empty()) {
            if (!q.under.is_absolute()) {
                return fail(ErrorCode::InvalidArgument, "index query: `under` must be absolute");
            }
            const Path rel =
                q.under.lexically_normal().lexically_relative(options_.root.lexically_normal());
            if (rel.empty()) {
                return fail(ErrorCode::InvalidArgument,
                            "index query: `under` is not below the index root");
            }
            for (const Path& part : rel) {
                const std::string& s = part.native();
                if (s == "..") {
                    return fail(ErrorCode::InvalidArgument,
                                "index query: `under` is not below the index root");
                }
                if (s.empty() || s == ".") {
                    continue;
                }
                comps.push_back(s);
            }
        }

        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "index query cancelled");
        }
        IndexResult result;
        const std::shared_lock lock(data_mu_);
        {
            const std::lock_guard sl(status_mu_);
            result.generation = status_.generation;
            result.complete = status_.state == IndexState::Ready;
        }
        std::uint32_t start = 0;
        for (const std::string& comp : comps) {
            const std::uint32_t slot = tree_.find_child(start, comp);
            if (slot == kNoIndex) {
                return result;
            }
            const std::uint32_t next = tree_.dirs[start].children[slot].dir;
            if (next == kNoIndex || !tree_.dirs[next].alive) {
                return result;
            }
            start = next;
        }

        struct Cand {
            std::int32_t score;
            std::uint32_t depth;
            std::uint32_t dir;
            std::uint32_t slot;
            std::uint32_t name_len;
        };
        std::vector<Cand> cands;
        std::string folded;
        std::vector<std::uint32_t> offsets;
        std::vector<std::uint32_t> cps;
        std::vector<std::int32_t> table;
        detail::Scratch scratch{folded, offsets, cps, table};
        std::string type_buffer;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> queue{{start, 0}};
        std::size_t visited = 0;
        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            const auto [d, depth] = queue[qi];
            const Dir& rec = tree_.dirs[d];
            for (std::uint32_t i = 0; i < rec.children.size(); ++i) {
                if (visited++ % kFilterStopCheckInterval == 0 && stop.stop_requested()) {
                    return fail(ErrorCode::Cancelled, "index query cancelled");
                }
                const Child& c = rec.children[i];
                if (!q.show_hidden && (c.flags & detail::kEntryHidden) != 0) {
                    continue;
                }
                if (c.dir != kNoIndex && tree_.dirs[c.dir].alive) {
                    queue.emplace_back(c.dir, depth + 1);
                }
                const std::string_view raw = Tree::raw_name(rec, c);
                if (!detail::allows_name(q.types, directory_like(c), raw, type_buffer)) {
                    continue;
                }
                const std::optional<std::int32_t> sc =
                    detail::evaluate(q.query, raw, scratch, nullptr, Tree::folded_name(rec, c));
                if (sc) {
                    cands.push_back(Cand{*sc, depth + 1, d, i, c.raw_len});
                }
            }
        }
        result.matched = cands.size();

        // Ranking: parent paths relative to `under`, computed once per directory on demand.
        std::unordered_map<std::uint32_t, std::string> rel_cache;
        const auto rel_of = [&](std::uint32_t d) -> const std::string& {
            auto it = rel_cache.find(d);
            if (it == rel_cache.end()) {
                it = rel_cache.emplace(d, tree_.relative_path(d, start)).first;
            }
            return it->second;
        };
        const auto info = [&](const Cand& c) {
            const Dir& rec = tree_.dirs[c.dir];
            return RankInfo{c.score, c.depth, Tree::raw_name(rec, rec.children[c.slot]),
                            std::string_view{}};
        };
        const auto before = [&](const Cand& a, const Cand& b) {
            // Steps 1-3 of ranks_before() on plain integers; names only for real ties.
            if (a.score != b.score) {
                return a.score > b.score;
            }
            if (a.depth != b.depth) {
                return a.depth < b.depth;
            }
            if (a.name_len != b.name_len) {
                return a.name_len < b.name_len;
            }
            RankInfo ia = info(a);
            RankInfo ib = info(b);
            if (ia.score == ib.score && ia.depth == ib.depth && ia.name == ib.name) {
                ia.parent = rel_of(a.dir);
                ib.parent = rel_of(b.dir);
            }
            return ranks_before(ia, ib);
        };
        const std::size_t keep =
            q.limit == 0 ? cands.size() : std::min<std::size_t>(q.limit, cands.size());
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "index query cancelled");
        }
        std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(keep),
                          cands.end(), before);
        cands.resize(keep);

        std::unordered_map<std::uint32_t, Path> abs_cache;
        result.hits.reserve(keep);
        for (const Cand& cand : cands) {
            const Dir& rec = tree_.dirs[cand.dir];
            const Child& c = rec.children[cand.slot];
            const std::string_view raw = Tree::raw_name(rec, c);
            auto it = abs_cache.find(cand.dir);
            if (it == abs_cache.end()) {
                it = abs_cache.emplace(cand.dir, tree_.path_of(cand.dir, options_.root)).first;
            }
            IndexHit hit;
            hit.path = it->second / std::string(raw);
            hit.kind = c.kind;
            hit.directory_like = directory_like(c);
            hit.depth = cand.depth;
            hit.match.score = cand.score;
            (void)detail::evaluate(q.query, raw, scratch, &hit.match.highlights);
            result.hits.push_back(std::move(hit));
        }
        return result;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "index query: out of memory");
    }
}

} // namespace

Result<std::unique_ptr<IIndex>> create_name_index(NameIndexOptions options) {
    if (options.root.empty() || !options.root.is_absolute()) {
        return fail(ErrorCode::InvalidArgument, "index root must be an absolute path");
    }
    if (options.max_entries == 0) {
        return fail(ErrorCode::InvalidArgument, "index max_entries must not be 0");
    }
    try {
        return std::unique_ptr<IIndex>(std::make_unique<NameIndex>(std::move(options)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "create_name_index: out of memory");
    }
}

} // namespace navigator::search
