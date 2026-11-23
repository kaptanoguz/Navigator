// src/preview/service.cpp: IPreviewService implementation: worker pool, priority queues
// (Inspector > Visible > Prefetch; newest epoch first, then submission order), coalescing of
// identical keys, cancellation by ticket or epoch, the Freedesktop disk cache in front of the
// providers, and a byte-budgeted LRU memory cache.
//
// All mutable state lives in one State object behind one mutex, shared (shared_ptr) by the
// service and its workers, so a worker detached at shutdown never touches freed memory.
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <unordered_map>

#include <navigator/preview_service.hpp>

#include "internal.hpp"

namespace navigator::preview {

namespace {

constexpr std::size_t kPriorityCount = 3;
constexpr std::size_t kMaxRegenRules = 64;
constexpr auto kShutdownWait = std::chrono::seconds(2);

struct Job {
    Ticket ticket;
    PreviewKey key;
    PreviewRequest request;
    RequestPriority priority = RequestPriority::Visible;
    std::uint64_t epoch = 0;
    std::uint64_t seq = 0;
    std::stop_source stop;
    bool running = false;
    bool cancelled = false;
};

struct JobOrder {
    bool operator()(const Job* a, const Job* b) const noexcept {
        if (a->epoch != b->epoch) {
            return a->epoch > b->epoch;
        }
        return a->seq < b->seq;
    }
};

struct CacheEntry {
    PreviewKey key;
    std::shared_ptr<const PreviewResult> result;
    std::size_t bytes = 0;
};

struct RegenRule {
    Path prefix;
    std::set<std::string> done;
};

struct State {
    // Immutable after construction.
    std::shared_ptr<const ProviderRegistry> registry;
    ThumbnailCacheOptions thumbs;
    std::size_t max_pending = kDefaultMaxPending;
    std::function<void()> on_completion;

    mutable std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable exit_cv;
    bool stopping = false;
    std::size_t live_workers = 0;
    std::vector<bool> exited;

    std::uint64_t next_ticket = 1;
    std::uint64_t next_seq = 1;
    std::set<Job*, JobOrder> pending[kPriorityCount];
    std::unordered_map<std::uint64_t, std::shared_ptr<Job>> jobs; // pending + running
    std::unordered_map<PreviewKey, Job*, PreviewKeyHash> by_key;  // coalescable jobs
    std::deque<Completion> done;
    std::size_t running = 0;

    std::list<CacheEntry> lru; // front = most recently used
    std::unordered_map<PreviewKey, std::list<CacheEntry>::iterator, PreviewKeyHash> cache_index;
    std::uint64_t cache_bytes = 0;
    std::uint64_t budget = kDefaultCacheBudgetBytes;
    std::vector<RegenRule> regen;

    std::uint64_t memory_hits = 0;
    std::uint64_t disk_hits = 0;
    std::uint64_t produced = 0;
    std::uint64_t thumbnails_written = 0;
    std::uint64_t failures_written = 0;

    // --- helpers, mutex held ---
    [[nodiscard]] std::size_t pending_count() const noexcept {
        std::size_t n = 0;
        for (const auto& q : pending) {
            n += q.size();
        }
        return n;
    }

    void forget(Job* job) {
        if (auto it = by_key.find(job->key); it != by_key.end() && it->second == job) {
            by_key.erase(it);
        }
        jobs.erase(job->ticket.value);
    }

    void drop_pending(Job* job) {
        pending[static_cast<std::size_t>(job->priority)].erase(job);
        forget(job);
    }

    void cancel_job(Job* job) {
        if (job->running) {
            job->cancelled = true;
            job->stop.request_stop();
            if (auto it = by_key.find(job->key); it != by_key.end() && it->second == job) {
                by_key.erase(it);
            }
        } else {
            drop_pending(job);
        }
    }

    void evict() {
        auto it = lru.end();
        while (cache_bytes > budget && it != lru.begin()) {
            --it;
            if (it->result.use_count() > 1) {
                continue; // held by the ui (or a queued completion)
            }
            cache_bytes -= it->bytes;
            cache_index.erase(it->key);
            it = lru.erase(it);
        }
    }

    void cache_insert(const PreviewKey& key, const std::shared_ptr<const PreviewResult>& result) {
        if (auto it = cache_index.find(key); it != cache_index.end()) {
            cache_bytes -= it->second->bytes;
            lru.erase(it->second);
            cache_index.erase(it);
        }
        CacheEntry e{key, result, approx_bytes(*result)};
        cache_bytes += e.bytes;
        lru.push_front(std::move(e));
        cache_index.emplace(key, lru.begin());
        evict();
    }

    // True once per path below a Regenerate prefix.
    bool consume_regen(const Path& path) {
        for (RegenRule& r : regen) {
            if (thumbnail::is_inside_cache(path, r.prefix) &&
                r.done.insert(path.lexically_normal().native()).second) {
                return true;
            }
        }
        return false;
    }
};

[[nodiscard]] ImageBuffer* result_image(PreviewResult& r) noexcept {
    if (auto* ip = std::get_if<ImagePreview>(&r.content)) {
        return &ip->image;
    }
    if (auto* vp = std::get_if<VideoPreview>(&r.content)) {
        return &vp->poster;
    }
    return nullptr;
}

Result<void> fit_into(ImageBuffer& image, PixelSize target) {
    if (image.empty() || target.empty()) {
        return ok();
    }
    const PixelSize fit = fit_size(image.size(), target);
    if (fit == image.size()) {
        return ok();
    }
    auto d = downscale(image, fit);
    if (!d) {
        return Unexpected<Error>(d.error());
    }
    image = std::move(*d);
    return ok();
}

[[nodiscard]] PreviewResult placeholder(PlaceholderReason reason, std::string detail) {
    PreviewResult r;
    r.content = PlaceholderPreview{reason, std::move(detail)};
    return r;
}

[[nodiscard]] bool is_video_mime(std::string_view mime) noexcept {
    return mime.substr(0, 6) == "video/";
}

// The job pipeline (worker thread, no lock held).
Result<PreviewResult> process(State& st, PreviewRequest req, bool regen) {
    if (req.mime.empty()) {
        req.mime = "application/octet-stream";
    }
    const ThumbnailCacheOptions& tc = st.thumbs;
    if (req.purpose != PreviewPurpose::Thumbnail || tc.root.empty()) {
        auto r = st.registry->produce(req);
        if (r) {
            const std::lock_guard lock(st.mutex);
            ++st.produced;
        }
        return r;
    }
    const thumbnail::Decision decision =
        thumbnail::decide(req.path, req.entry, req.location, tc.root, tc.policy);
    if (decision == thumbnail::Decision::Skip) {
        return placeholder(PlaceholderReason::NotGenerated, "thumbnail policy: skip");
    }
    auto target_meta = fs::stat(req.path, true);
    if (!target_meta) {
        return Unexpected<Error>(target_meta.error());
    }
    thumbnail::SourceInfo source;
    source.path = req.path;
    auto uri = thumbnail::file_uri(req.path);
    if (!uri) {
        return Unexpected<Error>(uri.error());
    }
    source.uri = std::move(*uri);
    source.mtime = target_meta->mtime;
    source.size = target_meta->size;
    source.mime = req.mime;
    const PixelSize wanted = req.target;
    const thumbnail::ThumbnailSize cls = thumbnail::size_for(std::max(wanted.width, wanted.height));
    if (tc.read && !regen) {
        auto found = thumbnail::lookup_thumbnail(tc.root, cls, source, req.stop);
        if (!found && found.error().code == ErrorCode::Cancelled) {
            return Unexpected<Error>(found.error());
        }
        if (found && found->status == thumbnail::LookupStatus::Valid) {
            ImageBuffer image = std::move(found->image);
            if (auto f = fit_into(image, wanted); !f) {
                return Unexpected<Error>(f.error());
            }
            PreviewResult r;
            if (is_video_mime(req.mime)) {
                VideoPreview v;
                v.poster = std::move(image);
                v.poster_origin = ImageOrigin::ThumbnailCache;
                if (const std::optional<std::int64_t> len = found->movie_length_s; len) {
                    v.info.duration_ms = *len * 1000;
                }
                r.content = std::move(v);
            } else {
                ImagePreview ip;
                ip.info.stored = found->original.empty() ? image.size() : found->original;
                ip.info.display = ip.info.stored;
                ip.image = std::move(image);
                ip.origin = ImageOrigin::ThumbnailCache;
                r.content = std::move(ip);
            }
            r.provider_id = "thumbnail.cache";
            const std::lock_guard lock(st.mutex);
            ++st.disk_hits;
            return r;
        }
        if (auto failed = thumbnail::has_failure(tc.root, source); failed && *failed) {
            return placeholder(PlaceholderReason::KnownFailure, "failure marker");
        }
    }
    if (decision == thumbnail::Decision::ReadOnly) {
        return placeholder(PlaceholderReason::NotGenerated, "thumbnail policy: read only");
    }
    req.target = {thumbnail::pixels(cls), thumbnail::pixels(cls)};
    auto r = st.registry->produce(req);
    if (!r) {
        if (tc.write && r.error().code == ErrorCode::InvalidArgument) {
            if (thumbnail::write_failure(tc.root, source)) {
                const std::lock_guard lock(st.mutex);
                ++st.failures_written;
            }
        }
        return r;
    }
    {
        const std::lock_guard lock(st.mutex);
        ++st.produced;
    }
    ImageBuffer* image = result_image(*r);
    if (image != nullptr && !image->empty()) {
        if (auto f = fit_into(*image, req.target); !f) {
            return Unexpected<Error>(f.error());
        }
        if (tc.write && !req.stop.stop_requested()) {
            thumbnail::WriteOptions wo;
            if (const auto* ip = std::get_if<ImagePreview>(&r->content)) {
                wo.original = ip->info.display;
            } else if (const auto* vp = std::get_if<VideoPreview>(&r->content)) {
                wo.original = vp->info.frame;
                if (vp->info.duration_ms) {
                    wo.movie_length_s = *vp->info.duration_ms / 1000;
                }
            }
            if (thumbnail::write_thumbnail(tc.root, cls, source, *image, wo)) {
                const std::lock_guard lock(st.mutex);
                ++st.thumbnails_written;
            }
        }
        if (auto f = fit_into(*image, wanted); !f) {
            return Unexpected<Error>(f.error());
        }
    }
    return r;
}

void worker_main(const std::shared_ptr<State>& state, std::size_t index) {
    State& st = *state;
    std::unique_lock lock(st.mutex);
    for (;;) {
        st.work_cv.wait(lock, [&] { return st.stopping || st.pending_count() > 0; });
        if (st.stopping) {
            break;
        }
        Job* job = nullptr;
        for (auto& q : st.pending) {
            if (!q.empty()) {
                job = *q.begin();
                q.erase(q.begin());
                break;
            }
        }
        if (job == nullptr) {
            continue;
        }
        const std::shared_ptr<Job> hold = st.jobs.at(job->ticket.value);
        job->running = true;
        ++st.running;
        PreviewRequest request = job->request;
        request.stop = job->stop.get_token();
        lock.unlock();

        bool regen = false;
        if (request.purpose == PreviewPurpose::Thumbnail) {
            const std::lock_guard g(st.mutex);
            regen = !st.regen.empty() && st.consume_regen(request.path);
        }
        Result<std::shared_ptr<const PreviewResult>> delivered =
            fail(ErrorCode::Unknown, "not run");
        bool discard = false;
        try {
            auto r = process(st, std::move(request), regen);
            if (r) {
                delivered = std::shared_ptr<const PreviewResult>(
                    std::make_shared<PreviewResult>(std::move(*r)));
            } else {
                discard = r.error().code == ErrorCode::Cancelled;
                delivered = Unexpected<Error>(r.error());
            }
        } catch (const std::bad_alloc&) {
            delivered = fail(ErrorCode::OutOfMemory, "preview job: allocation failed");
        } catch (...) {
            delivered = fail(ErrorCode::Unknown, "preview job: unexpected exception");
        }

        lock.lock();
        --st.running;
        discard = discard || job->cancelled || st.stopping;
        bool notify = false;
        if (!discard) {
            if (delivered) {
                try {
                    st.cache_insert(job->key, *delivered);
                } catch (const std::bad_alloc&) {
                    // Not cached; the completion below still delivers the result.
                    (void)0;
                }
            }
            try {
                st.done.push_back(Completion{job->ticket, job->key, std::move(delivered)});
                notify = static_cast<bool>(st.on_completion);
            } catch (const std::bad_alloc&) {
                notify = false; // completion dropped; the ui resubmits visible rows
            }
        }
        st.forget(job);
        if (notify) {
            lock.unlock();
            st.on_completion();
            lock.lock();
        }
    }
    --st.live_workers;
    st.exited[index] = true;
    st.exit_cv.notify_all();
}

class PreviewService final : public IPreviewService {
public:
    explicit PreviewService(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~PreviewService() override { shutdown(); }
    PreviewService(const PreviewService&) = delete;
    PreviewService& operator=(const PreviewService&) = delete;
    PreviewService(PreviewService&&) = delete;
    PreviewService& operator=(PreviewService&&) = delete;

    Result<void> start(std::size_t workers) {
        try {
            const std::lock_guard lock(state_->mutex);
            state_->exited.assign(workers, false);
            for (std::size_t i = 0; i < workers; ++i) {
                std::shared_ptr<State> s = state_;
                threads_.emplace_back([s, i] { worker_main(s, i); });
                ++state_->live_workers;
            }
        } catch (const std::system_error& e) {
            return fail(ErrorCode::PlatformError,
                        std::string("thread creation failed: ") + e.what());
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "thread creation failed");
        }
        return ok();
    }

    Ticket submit(PreviewRequest request, RequestPriority priority, std::uint64_t epoch) override {
        if (request.path.empty() || !request.path.is_absolute()) {
            return Ticket{};
        }
        State& st = *state_;
        try {
            const PreviewKey key = make_key(request);
            bool notify = false;
            Ticket ticket;
            {
                const std::lock_guard lock(st.mutex);
                if (st.stopping) {
                    return Ticket{};
                }
                if (auto hit = st.cache_index.find(key); hit != st.cache_index.end()) {
                    st.lru.splice(st.lru.begin(), st.lru, hit->second);
                    ++st.memory_hits;
                    ticket = Ticket{st.next_ticket++};
                    st.done.push_back(Completion{ticket, key, hit->second->result});
                    notify = static_cast<bool>(st.on_completion);
                } else if (auto it = st.by_key.find(key); it != st.by_key.end()) {
                    Job* job = it->second;
                    const auto old_prio = static_cast<std::size_t>(job->priority);
                    const RequestPriority new_prio = std::min(job->priority, priority);
                    const std::uint64_t new_epoch = std::max(job->epoch, epoch);
                    if (!job->running) {
                        st.pending[old_prio].erase(job);
                    }
                    job->priority = new_prio;
                    job->epoch = new_epoch;
                    if (!job->running) {
                        st.pending[static_cast<std::size_t>(new_prio)].insert(job);
                    }
                    return job->ticket;
                } else {
                    auto job = std::make_shared<Job>();
                    job->ticket = Ticket{st.next_ticket++};
                    job->key = key;
                    job->request = std::move(request);
                    job->priority = priority;
                    job->epoch = epoch;
                    job->seq = st.next_seq++;
                    ticket = job->ticket;
                    Job* raw = job.get();
                    st.jobs.emplace(ticket.value, std::move(job));
                    st.by_key.emplace(key, raw);
                    st.pending[static_cast<std::size_t>(priority)].insert(raw);
                    enforce_max_pending(st);
                    st.work_cv.notify_one();
                }
            }
            if (notify) {
                st.on_completion();
            }
            return ticket;
        } catch (const std::bad_alloc&) {
            return Ticket{};
        }
    }

    [[nodiscard]] std::shared_ptr<const PreviewResult>
    lookup(const PreviewKey& key) const override {
        State& st = *state_;
        const std::lock_guard lock(st.mutex);
        auto hit = st.cache_index.find(key);
        if (hit == st.cache_index.end()) {
            return nullptr;
        }
        st.lru.splice(st.lru.begin(), st.lru, hit->second);
        ++st.memory_hits;
        return hit->second->result;
    }

    void cancel(Ticket ticket) noexcept override {
        State& st = *state_;
        const std::lock_guard lock(st.mutex);
        if (auto it = st.jobs.find(ticket.value); it != st.jobs.end()) {
            const std::shared_ptr<Job> hold = it->second;
            st.cancel_job(hold.get());
        }
    }

    void cancel_older(RequestPriority priority, std::uint64_t epoch) noexcept override {
        State& st = *state_;
        const std::lock_guard lock(st.mutex);
        std::vector<std::shared_ptr<Job>> victims;
        try {
            for (const auto& [t, job] : st.jobs) {
                if (job->priority == priority && job->epoch < epoch && !job->cancelled) {
                    victims.push_back(job);
                }
            }
        } catch (const std::bad_alloc&) {
            return;
        }
        for (const auto& j : victims) {
            st.cancel_job(j.get());
        }
    }

    std::size_t poll(std::vector<Completion>& out) override {
        State& st = *state_;
        const std::lock_guard lock(st.mutex);
        const std::size_t n = st.done.size();
        out.reserve(out.size() + n);
        for (Completion& c : st.done) {
            out.push_back(std::move(c));
        }
        st.done.clear();
        return n;
    }

    void invalidate(const Path& path, InvalidateMode mode) override {
        State& st = *state_;
        const std::lock_guard lock(st.mutex);
        for (auto it = st.lru.begin(); it != st.lru.end();) {
            if (thumbnail::is_inside_cache(it->key.path, path)) {
                st.cache_bytes -= it->bytes;
                st.cache_index.erase(it->key);
                it = st.lru.erase(it);
            } else {
                ++it;
            }
        }
        std::vector<Job*> victims;
        for (const auto& q : st.pending) {
            for (Job* j : q) {
                if (thumbnail::is_inside_cache(j->request.path, path)) {
                    victims.push_back(j);
                }
            }
        }
        for (Job* j : victims) {
            st.drop_pending(j);
        }
        if (mode == InvalidateMode::Regenerate) {
            if (st.regen.size() >= kMaxRegenRules) {
                st.regen.erase(st.regen.begin());
            }
            st.regen.push_back(RegenRule{path, {}});
        }
    }

    void set_cache_budget(std::uint64_t bytes) override {
        State& st = *state_;
        const std::lock_guard lock(st.mutex);
        st.budget = std::max(bytes, kMinCacheBudgetBytes);
        st.evict();
    }

    void clear_cache() noexcept override {
        State& st = *state_;
        const std::lock_guard lock(st.mutex);
        st.lru.clear();
        st.cache_index.clear();
        st.cache_bytes = 0;
    }

    [[nodiscard]] ServiceStats stats() const override {
        const State& st = *state_;
        const std::lock_guard lock(st.mutex);
        ServiceStats s;
        s.cache_bytes = st.cache_bytes;
        s.cache_budget_bytes = st.budget;
        s.cache_entries = st.lru.size();
        s.pending = st.pending_count();
        s.running = st.running;
        s.memory_hits = st.memory_hits;
        s.disk_hits = st.disk_hits;
        s.produced = st.produced;
        s.thumbnails_written = st.thumbnails_written;
        s.failures_written = st.failures_written;
        return s;
    }

    void shutdown() noexcept override {
        State& st = *state_;
        std::unique_lock lock(st.mutex);
        if (st.stopping && threads_.empty()) {
            return;
        }
        st.stopping = true;
        for (auto& q : st.pending) {
            q.clear();
        }
        for (auto& [t, job] : st.jobs) {
            job->cancelled = true;
            job->stop.request_stop();
        }
        st.by_key.clear();
        st.work_cv.notify_all();
        (void)st.exit_cv.wait_for(lock, kShutdownWait, [&] { return st.live_workers == 0; });
        const std::vector<bool> exited = st.exited;
        lock.unlock();
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            if (!threads_[i].joinable()) {
                continue;
            }
            if (i < exited.size() && exited[i]) {
                threads_[i].join();
            } else {
                threads_[i].detach(); // stuck in a blocking read; it holds its own State ref
            }
        }
        threads_.clear();
    }

private:
    static void enforce_max_pending(State& st) {
        while (st.pending_count() > st.max_pending) {
            bool dropped = false;
            for (const RequestPriority p : {RequestPriority::Prefetch, RequestPriority::Visible}) {
                auto& q = st.pending[static_cast<std::size_t>(p)];
                if (!q.empty()) {
                    Job* oldest = *std::prev(q.end());
                    st.drop_pending(oldest);
                    dropped = true;
                    break;
                }
            }
            if (!dropped) {
                break;
            }
        }
    }

    std::shared_ptr<State> state_;
    std::vector<std::thread> threads_;
};

} // namespace

Result<std::unique_ptr<IPreviewService>> create_preview_service(PreviewServiceOptions options) {
    if (!options.registry) {
        return fail(ErrorCode::InvalidArgument, "create_preview_service: no registry");
    }
    try {
        auto state = std::make_shared<State>();
        state->registry = std::move(options.registry);
        state->thumbs = std::move(options.thumbnails);
        state->max_pending = std::max<std::size_t>(options.max_pending, 1);
        state->on_completion = std::move(options.on_completion);
        state->budget = std::max(options.cache_budget_bytes, kMinCacheBudgetBytes);
        std::size_t workers = options.worker_count;
        if (workers == 0) {
            const unsigned hc = std::thread::hardware_concurrency();
            workers = std::clamp<std::size_t>(hc / 2U, 2, 6);
        }
        auto service = std::make_unique<PreviewService>(state);
        if (auto r = service->start(workers); !r) {
            service->shutdown();
            return Unexpected<Error>(r.error());
        }
        return std::unique_ptr<IPreviewService>(std::move(service));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "create_preview_service: allocation failed");
    }
}

} // namespace navigator::preview
