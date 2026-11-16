// src/ui/browser/watch_worker.cpp — see watch_worker.hpp.
#include "browser/watch_worker.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace navigator::ui {

WatchWorker::WatchWorker(WatcherFactory factory, std::function<void()> wake)
    : factory_(std::move(factory)), wake_(std::move(wake)),
      wake_fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

WatchWorker::~WatchWorker() {
    thread_.request_stop(); // the stop callback in run() signals the eventfd
    if (thread_.joinable()) {
        thread_.join();
    }
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
    }
}

void WatchWorker::signal() const noexcept {
    if (wake_fd_ >= 0) {
        const std::uint64_t one = 1;
        (void)!::write(wake_fd_, &one, sizeof one);
    }
}

bool WatchWorker::wait_events(IWatcher& watcher, std::chrono::milliseconds timeout) const {
    const int fd = watcher.native_handle();
    if (fd < 0 || wake_fd_ < 0) {
        return watcher.wait(timeout.count() < 0 ? kPollInterval : timeout);
    }
    std::array<pollfd, 2> fds{pollfd{fd, POLLIN, 0}, pollfd{wake_fd_, POLLIN, 0}};
    const int n = ::poll(fds.data(), fds.size(), static_cast<int>(timeout.count()));
    if (n <= 0) {
        return false; // timeout or EINTR: the caller loops
    }
    if ((fds[1].revents & POLLIN) != 0) {
        std::uint64_t drained = 0;
        (void)!::read(wake_fd_, &drained, sizeof drained);
        return false; // watch()/unwatch() or stop: the caller re-checks
    }
    return (fds[0].revents & POLLIN) != 0;
}

void WatchWorker::watch(Path dir, std::uint64_t tag) {
    {
        const std::scoped_lock lock(mutex_);
        ops_.push_back(Op{true, std::move(dir), tag});
        ++queued_;
    }
    target_cv_.notify_one();
    signal();
}

void WatchWorker::unwatch(std::uint64_t tag) {
    {
        const std::scoped_lock lock(mutex_);
        ops_.push_back(Op{false, {}, tag});
        ++queued_;
    }
    target_cv_.notify_one();
    signal();
}

std::vector<WatchBatch> WatchWorker::take() {
    const std::scoped_lock lock(mutex_);
    std::vector<WatchBatch> out;
    out.swap(batches_);
    return out;
}

bool WatchWorker::wait_for_batch(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return batch_cv_.wait_for(lock, timeout, [this] { return !batches_.empty(); });
}

bool WatchWorker::wait_for_tag(std::uint64_t tag, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return batch_cv_.wait_for(lock, timeout, [&] { return applied_tag_ >= tag; });
}

bool WatchWorker::wait_for_target(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return batch_cv_.wait_for(lock, timeout, [this] { return applied_ == queued_; });
}

WatchBatch WatchWorker::build_batch(const std::vector<WatchEvent>& events, WatchHandle handle,
                                    const Path& dir, std::uint64_t tag) const {
    WatchBatch batch;
    batch.tag = tag;
    // Stat each name once, after the burst: the final state is what the list must show.
    std::unordered_map<std::string, std::optional<fs::DirEntry>> stats;
    const auto fresh = [&](const std::string& name) -> const std::optional<fs::DirEntry>& {
        auto it = stats.find(name);
        if (it == stats.end()) {
            auto st = fs::stat(dir / name, false);
            std::optional<fs::DirEntry> value;
            if (st) {
                st->name = name;
                st->hidden = fs::is_hidden_name(name);
                value = std::move(*st);
            }
            it = stats.emplace(name, std::move(value)).first;
        }
        return it->second;
    };
    for (const WatchEvent& ev : events) {
        if (ev.kind == WatchEventKind::Overflow) {
            batch.overflow = true;
            continue;
        }
        if (ev.handle != handle) {
            continue; // an earlier target's leftovers
        }
        switch (ev.kind) {
        case WatchEventKind::WatchedDirGone: batch.gone = true; break;
        case WatchEventKind::Deleted:
        case WatchEventKind::MovedFrom:
            batch.changes.push_back(WatchChange{WatchChange::Kind::Remove, ev.name, {}, {}});
            break;
        case WatchEventKind::Created:
        case WatchEventKind::MovedTo:
        case WatchEventKind::Modified:
        case WatchEventKind::Attrib: {
            if (ev.name.empty()) {
                break; // the directory's own attributes
            }
            const auto& st = fresh(ev.name);
            if (st) {
                batch.changes.push_back(WatchChange{WatchChange::Kind::Upsert, ev.name, {}, st});
            } else {
                batch.changes.push_back(WatchChange{WatchChange::Kind::Remove, ev.name, {}, {}});
            }
            break;
        }
        case WatchEventKind::Renamed:
            batch.changes.push_back(
                WatchChange{WatchChange::Kind::Rename, ev.name, ev.old_name, fresh(ev.name)});
            break;
        case WatchEventKind::Overflow: break;
        }
    }
    return batch;
}

void WatchWorker::run(const std::stop_token& stop) {
    std::unique_ptr<IWatcher> watcher;
    if (factory_) {
        if (auto w = factory_(); w) {
            watcher = std::move(*w);
        }
    }
    const std::stop_callback on_stop(stop, [this] { signal(); });
    std::vector<Target> targets;
    std::vector<WatchEvent> events;
    const auto watching_any = [&] {
        return std::any_of(targets.begin(), targets.end(),
                           [](const Target& t) { return t.handle != kInvalidWatchHandle; });
    };

    while (!stop.stop_requested()) {
        // 1. Apply queued watch()/unwatch() calls.
        std::vector<Op> ops;
        {
            std::unique_lock lock(mutex_);
            if (ops_.empty() && !watching_any()) {
                // Nothing to watch: sleep until watch() or shutdown.
                if (!target_cv_.wait(lock, stop, [this] { return !ops_.empty(); })) {
                    break;
                }
            }
            ops.swap(ops_);
        }
        if (!ops.empty()) {
            std::vector<WatchBatch> ready;
            std::uint64_t newest_tag = 0;
            for (Op& op : ops) {
                if (op.add) {
                    Target t{op.tag, std::move(op.dir), kInvalidWatchHandle};
                    if (watcher && !t.dir.empty()) {
                        if (auto h = watcher->add(t.dir); h) {
                            t.handle = *h;
                        }
                    }
                    WatchBatch marker;
                    marker.tag = t.tag;
                    marker.ready = true;
                    marker.watching = t.handle != kInvalidWatchHandle;
                    marker.at = std::chrono::steady_clock::now();
                    ready.push_back(std::move(marker));
                    newest_tag = std::max(newest_tag, t.tag);
                    targets.push_back(std::move(t));
                } else {
                    const auto it = std::find_if(targets.begin(), targets.end(),
                                                 [&](const Target& t) { return t.tag == op.tag; });
                    if (it != targets.end()) {
                        if (watcher && it->handle != kInvalidWatchHandle) {
                            watcher->remove(it->handle);
                        }
                        targets.erase(it);
                    }
                }
            }
            {
                const std::scoped_lock lock(mutex_);
                for (WatchBatch& b : ready) {
                    batches_.push_back(std::move(b));
                }
                applied_ += ops.size();
                applied_tag_ = std::max(applied_tag_, newest_tag);
            }
            batch_cv_.notify_all();
            if (!ready.empty() && wake_) {
                wake_();
            }
            continue;
        }

        // 2. Wait for events (or a new op / shutdown), then coalesce the burst.
        if (!wait_events(*watcher, std::chrono::milliseconds(-1))) {
            continue;
        }
        events.clear();
        (void)watcher->poll(events);
        const auto deadline = std::chrono::steady_clock::now() + kCoalesce;
        while (!stop.stop_requested()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            // A watch()/unwatch() ends the burst early (the batch still goes out), so a new
            // folder's watch is never held up by coalescing.
            if (left.count() <= 0 || !wait_events(*watcher, left)) {
                break;
            }
            (void)watcher->poll(events);
        }

        // 3. One batch per target.
        std::vector<WatchBatch> out;
        std::vector<WatchHandle> gone;
        for (const Target& t : targets) {
            if (t.handle == kInvalidWatchHandle) {
                continue;
            }
            WatchBatch batch = build_batch(events, t.handle, t.dir, t.tag);
            if (batch.gone) {
                gone.push_back(t.handle);
            }
            if (!batch.changes.empty() || batch.overflow || batch.gone) {
                out.push_back(std::move(batch));
            }
        }
        for (Target& t : targets) {
            if (std::find(gone.begin(), gone.end(), t.handle) != gone.end()) {
                t.handle = kInvalidWatchHandle; // the backend already removed it
            }
        }
        if (out.empty()) {
            continue;
        }
        {
            const std::scoped_lock lock(mutex_);
            for (WatchBatch& b : out) {
                batches_.push_back(std::move(b));
            }
        }
        batch_cv_.notify_all();
        if (wake_) {
            wake_();
        }
    }
    for (const Target& t : targets) {
        if (watcher && t.handle != kInvalidWatchHandle) {
            watcher->remove(t.handle);
        }
    }
}

} // namespace navigator::ui
