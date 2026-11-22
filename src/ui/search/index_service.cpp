// src/ui/search/index_service.cpp — see index_service.hpp.
#include "search/index_service.hpp"

#include <condition_variable>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>

namespace navigator::ui {
namespace {

// True when `base` is a component-wise prefix of `p` (both lexically normal and absolute).
bool path_within(const Path& p, const Path& base) {
    auto pi = p.begin();
    for (auto bi = base.begin(); bi != base.end(); ++bi, ++pi) {
        if (bi->empty()) {
            continue; // trailing separator
        }
        if (pi == p.end() || *pi != *bi) {
            return false;
        }
    }
    return true;
}

std::size_t depth_of(const Path& p) {
    std::size_t n = 0;
    for (const Path& part : p) {
        if (!part.empty()) {
            ++n;
        }
    }
    return n;
}

} // namespace

IndexService::IndexService(Options options) : options_(std::move(options)) {
    if (!options_.factory) {
        options_.factory = [](search::NameIndexOptions o) {
            return search::create_name_index(std::move(o));
        };
    }
}

IndexService::~IndexService() { shutdown(std::chrono::milliseconds(2000)); }

bool IndexService::create(const Path& root, const VolumeIdentity& identity, const std::string& id) {
    search::NameIndexOptions o;
    o.root = root;
    o.identity = identity;
    if (!options_.cache_root.empty()) {
        o.file = search::index_file(options_.cache_root, identity);
    }
    o.watcher_factory = options_.watcher_factory;
    o.watch_budget = options_.watch_budget;
    o.background_priority = true;
    o.on_status_change = [flag = changed_, wake = options_.wake] {
        // Coalesce: one wake per frame is enough.
        if (!flag->exchange(true, std::memory_order_acq_rel) && wake) {
            wake();
        }
    };
    Result<std::unique_ptr<search::IIndex>> made = options_.factory(std::move(o));
    if (!made) {
        return false;
    }
    std::shared_ptr<search::IIndex> index(std::move(*made));
    if (started_) {
        index->start();
    }
    indexes_[id] = Entry{root, std::move(index)};
    return true;
}

bool IndexService::set_volumes(const std::vector<VolumeInfo>& volumes) {
    if (!options_.enabled || shut_down_) {
        return false;
    }
    bool changed = false;
    std::set<std::string> alive;
    for (const VolumeInfo& v : volumes) {
        if (!search::should_index_by_default(v) || v.mount_point.empty() ||
            !v.mount_point.is_absolute()) {
            continue;
        }
        const VolumeIdentity identity = identity_of(v);
        const std::string id = search::index_stable_id(identity);
        alive.insert(id);
        const auto it = indexes_.find(id);
        if (it != indexes_.end() && it->second.root == v.mount_point) {
            continue;
        }
        if (it != indexes_.end()) {
            retire(std::move(it->second.index)); // remounted elsewhere: a fresh index
            indexes_.erase(it);
        }
        changed = create(v.mount_point, identity, id) || changed;
    }
    for (auto it = indexes_.begin(); it != indexes_.end();) {
        if (alive.contains(it->first) || it->first.starts_with("debug:")) {
            ++it;
            continue;
        }
        retire(std::move(it->second.index));
        it = indexes_.erase(it);
        changed = true;
    }
    return changed;
}

bool IndexService::add_root(const Path& root, const VolumeIdentity& identity) {
    if (!options_.enabled || shut_down_ || !root.is_absolute()) {
        return false;
    }
    return create(root, identity, "debug:" + search::index_stable_id(identity));
}

void IndexService::start() {
    if (started_ || shut_down_) {
        return;
    }
    started_ = true;
    for (auto& [id, e] : indexes_) {
        e.index->start();
    }
}

std::shared_ptr<search::IIndex> IndexService::index_for(const Path& dir) const {
    const Entry* best = nullptr;
    std::size_t best_depth = 0;
    for (const auto& [id, e] : indexes_) {
        if (path_within(dir, e.root)) {
            const std::size_t d = depth_of(e.root);
            if (best == nullptr || d > best_depth) {
                best = &e;
                best_depth = d;
            }
        }
    }
    return best != nullptr ? best->index : nullptr;
}

bool IndexService::rebuild(const Path& dir) const {
    const std::shared_ptr<search::IIndex> index = index_for(dir);
    if (!index) {
        return false;
    }
    index->request_rescan(search::RescanKind::Rebuild);
    return true;
}

bool IndexService::frame_start() noexcept {
    return changed_->exchange(false, std::memory_order_acq_rel);
}

void IndexService::retire(std::shared_ptr<search::IIndex> index) {
    if (!index) {
        return;
    }
    // stop() saves and joins the index's own thread: never on the UI thread.
    try {
        reapers_.emplace_back([i = index] { i->stop(); });
    } catch (const std::system_error&) {
        index->stop(); // no thread to be had: stop inline rather than leave it running
    }
}

void IndexService::shutdown(std::chrono::milliseconds budget) {
    if (shut_down_) {
        return;
    }
    shut_down_ = true;
    // Every index stops in parallel with the retired ones; the UI thread waits at most
    // `budget` and then detaches what is still saving.
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        std::size_t left = 0;
    };
    auto gate = std::make_shared<Gate>();
    std::vector<std::thread> stoppers;
    std::vector<std::shared_ptr<search::IIndex>> live;
    live.reserve(indexes_.size());
    for (auto& [id, e] : indexes_) {
        live.push_back(std::move(e.index));
    }
    indexes_.clear();
    std::vector<std::thread> reapers;
    reapers.swap(reapers_);
    gate->left = live.size() + reapers.size();
    const auto done = [gate] {
        {
            const std::scoped_lock lock(gate->m);
            --gate->left;
        }
        gate->cv.notify_all();
    };
    try {
        for (std::shared_ptr<search::IIndex>& i : live) {
            stoppers.emplace_back([i = std::move(i), done] {
                i->stop();
                done();
            });
        }
        for (std::thread& r : reapers) {
            stoppers.emplace_back([t = std::move(r), done]() mutable {
                if (t.joinable()) {
                    t.join();
                }
                done();
            });
        }
    } catch (const std::system_error&) {
        // Out of threads: whatever did not get one is stopped by its destructor below.
        const std::scoped_lock lock(gate->m);
        gate->left = 0;
    }
    {
        std::unique_lock lock(gate->m);
        (void)gate->cv.wait_for(lock, budget, [&] { return gate->left == 0; });
    }
    for (std::thread& r : reapers) {
        if (r.joinable()) {
            r.detach(); // only when no helper thread could be started to join it
        }
    }
    for (std::thread& t : stoppers) {
        t.detach(); // finished ones exit at once; a slow save finishes on its own
    }
}

} // namespace navigator::ui
