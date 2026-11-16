// src/ui/browser/watch_worker.cpp — see watch_worker.hpp.
#include "browser/watch_worker.hpp"

#include <unordered_map>
#include <utility>

namespace navigator::ui {

WatchWorker::WatchWorker(WatcherFactory factory, std::function<void()> wake)
    : factory_(std::move(factory)), wake_(std::move(wake)),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

WatchWorker::~WatchWorker() { thread_.request_stop(); }

void WatchWorker::retarget(Path dir, std::uint64_t tag) {
    {
        const std::scoped_lock lock(mutex_);
        target_.dir = std::move(dir);
        target_.tag = tag;
        ++target_.serial;
    }
    target_cv_.notify_one();
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

bool WatchWorker::wait_for_target(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return batch_cv_.wait_for(lock, timeout, [this] { return applied_ == target_.serial; });
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
    WatchHandle handle = kInvalidWatchHandle;
    Path dir;
    std::uint64_t tag = 0;
    std::uint64_t serial = 0;
    std::vector<WatchEvent> events;

    while (!stop.stop_requested()) {
        // Apply a new target.
        {
            std::unique_lock lock(mutex_);
            if (handle == kInvalidWatchHandle && target_.serial == serial) {
                // Nothing to watch: sleep until retarget() or shutdown.
                applied_ = serial;
                batch_cv_.notify_all();
                if (!target_cv_.wait(lock, stop, [&] { return target_.serial != serial; })) {
                    break;
                }
            }
            if (target_.serial != serial) {
                const Target next = target_;
                lock.unlock();
                if (watcher && handle != kInvalidWatchHandle) {
                    watcher->remove(handle);
                }
                handle = kInvalidWatchHandle;
                dir = next.dir;
                tag = next.tag;
                serial = next.serial;
                if (watcher && !dir.empty()) {
                    if (auto h = watcher->add(dir); h) {
                        handle = *h;
                    }
                }
                lock.lock();
                applied_ = serial;
                batch_cv_.notify_all();
                continue;
            }
        }

        if (!watcher->wait(kPollInterval)) {
            continue;
        }
        events.clear();
        (void)watcher->poll(events);
        // Coalesce the burst: keep draining while events keep arriving within the window.
        const auto deadline = std::chrono::steady_clock::now() + kCoalesce;
        while (!stop.stop_requested()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0 || !watcher->wait(left)) {
                break;
            }
            (void)watcher->poll(events);
        }
        WatchBatch batch = build_batch(events, handle, dir, tag);
        if (batch.gone) {
            handle = kInvalidWatchHandle; // already removed by the backend
        }
        if (batch.changes.empty() && !batch.overflow && !batch.gone) {
            continue;
        }
        {
            const std::scoped_lock lock(mutex_);
            batches_.push_back(std::move(batch));
        }
        batch_cv_.notify_all();
        if (wake_) {
            wake_();
        }
    }
    if (watcher && handle != kInvalidWatchHandle) {
        watcher->remove(handle);
    }
}

} // namespace navigator::ui
