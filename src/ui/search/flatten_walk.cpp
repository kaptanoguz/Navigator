// src/ui/search/flatten_walk.cpp — see flatten_walk.hpp.
#include "search/flatten_walk.hpp"

#include <condition_variable>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

namespace navigator::ui {
namespace {

std::atomic<int> g_live_walks{0};

class FsWalkSource final : public WalkSource {
public:
    explicit FsWalkSource(fs::Walker walker) : walker_(std::move(walker)) {}
    Result<bool> next_batch(fs::WalkBatch& out, std::stop_token stop) override {
        return walker_.next_batch(out, std::move(stop));
    }
    [[nodiscard]] const fs::WalkStats& stats() const noexcept override { return walker_.stats(); }

private:
    fs::Walker walker_;
};

} // namespace

WalkFactory default_walk_factory() {
    return [](const fs::WalkOptions& options) -> Result<std::unique_ptr<WalkSource>> {
        Result<fs::Walker> w = fs::Walker::open(options);
        if (!w) {
            return fail(w.error().code, w.error().message);
        }
        return std::unique_ptr<WalkSource>(std::make_unique<FsWalkSource>(std::move(*w)));
    };
}

struct FlattenWalk::Shared {
    std::mutex mutex;
    std::condition_variable finished_cv;
    std::vector<FlatChunkPtr> chunks;
    fs::WalkStats stats;
    bool finished = false;
    bool cancelled_seen = false;
    std::optional<Error> error;
    std::function<void()> wake; // cleared by cancel(): a detached thread never wakes a dead loop
    std::stop_source stop;
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> exited{false};

    void notify() {
        std::function<void()> w;
        {
            const std::scoped_lock lock(mutex);
            w = wake;
        }
        if (w) {
            w();
        }
    }
};

namespace {

void run_walk(const std::shared_ptr<FlattenWalk::Shared>& sh, const fs::WalkOptions& options,
              const WalkFactory& factory);

} // namespace

FlattenWalk::FlattenWalk(fs::WalkOptions options, WalkFactory factory, std::function<void()> wake)
    : shared_(std::make_shared<Shared>()) {
    shared_->wake = std::move(wake);
    if (!factory) {
        factory = default_walk_factory();
    }
    g_live_walks.fetch_add(1, std::memory_order_acq_rel);
    try {
        std::thread([sh = shared_, opts = std::move(options), f = std::move(factory)] {
            run_walk(sh, opts, f);
            sh->exited.store(true, std::memory_order_release);
            g_live_walks.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    } catch (const std::system_error&) {
        g_live_walks.fetch_sub(1, std::memory_order_acq_rel);
        const std::scoped_lock lock(shared_->mutex);
        shared_->finished = true;
        shared_->error = Error{ErrorCode::PlatformError, "could not start the walk thread"};
        shared_->exited.store(true, std::memory_order_release);
    }
}

FlattenWalk::~FlattenWalk() { cancel(); }

void FlattenWalk::cancel() noexcept {
    if (!shared_) {
        return;
    }
    shared_->cancel_requested.store(true, std::memory_order_release);
    shared_->stop.request_stop();
    const std::scoped_lock lock(shared_->mutex);
    shared_->wake = nullptr;
}

bool FlattenWalk::cancelled() const noexcept {
    return shared_ && shared_->cancel_requested.load(std::memory_order_acquire);
}

FlattenWalk::Update FlattenWalk::take() {
    Update u;
    const std::scoped_lock lock(shared_->mutex);
    u.chunks.swap(shared_->chunks);
    u.stats = shared_->stats;
    u.finished = shared_->finished;
    u.cancelled = shared_->cancelled_seen || cancelled();
    u.error = shared_->error;
    return u;
}

bool FlattenWalk::exited() const noexcept {
    return shared_->exited.load(std::memory_order_acquire);
}

bool FlattenWalk::wait_finished(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(shared_->mutex);
    return shared_->finished_cv.wait_for(lock, timeout, [this] { return shared_->finished; });
}

int FlattenWalk::live_threads() noexcept { return g_live_walks.load(std::memory_order_acquire); }

bool FlattenWalk::wait_for_exit(std::chrono::milliseconds timeout) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (live_threads() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

namespace {

void finish(FlattenWalk::Shared& sh, std::optional<Error> error, bool cancelled) {
    {
        const std::scoped_lock lock(sh.mutex);
        sh.finished = true;
        sh.cancelled_seen = cancelled;
        if (error) {
            sh.error = std::move(error);
        }
    }
    sh.finished_cv.notify_all();
    sh.notify();
}

void run_walk(const std::shared_ptr<FlattenWalk::Shared>& sh, const fs::WalkOptions& options,
              const WalkFactory& factory) {
    const std::stop_token stop = sh->stop.get_token();
    try {
        Result<std::unique_ptr<WalkSource>> source = factory(options);
        if (!source) {
            finish(*sh, source.error(), false);
            return;
        }
        WalkSource& walker = **source;
        std::uint32_t next_entry = 0;
        fs::WalkBatch batch;
        bool more = true;
        while (more) {
            if (stop.stop_requested()) {
                finish(*sh, std::nullopt, true);
                return;
            }
            Result<bool> r = walker.next_batch(batch, stop);
            if (!r) {
                const bool cancelled = r.error().code == ErrorCode::Cancelled;
                {
                    const std::scoped_lock lock(sh->mutex);
                    sh->stats = walker.stats();
                }
                finish(*sh, cancelled ? std::nullopt : std::optional<Error>(r.error()), cancelled);
                return;
            }
            more = *r;
            bool was_empty = false;
            if (!batch.entries.empty() || !batch.new_dirs.empty()) {
                auto chunk = std::make_shared<FlatChunk>();
                chunk->first_entry = next_entry;
                chunk->first_dir = batch.first_dir;
                chunk->new_dirs = std::move(batch.new_dirs);
                chunk->entries = std::move(batch.entries);
                next_entry += static_cast<std::uint32_t>(chunk->entries.size());
                const std::scoped_lock lock(sh->mutex);
                was_empty = sh->chunks.empty();
                sh->chunks.push_back(std::move(chunk));
                sh->stats = walker.stats();
            } else {
                const std::scoped_lock lock(sh->mutex);
                sh->stats = walker.stats();
            }
            batch.clear();
            // One wake per drained mailbox: the UI takes everything at its next frame start.
            if (was_empty && more) {
                sh->notify();
            }
        }
        finish(*sh, std::nullopt, false);
    } catch (const std::bad_alloc&) {
        finish(*sh, Error{ErrorCode::OutOfMemory, "flatten: out of memory"}, false);
    }
}

} // namespace

} // namespace navigator::ui
