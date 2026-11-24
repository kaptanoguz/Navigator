// src/ui/browser/tree_model.cpp — virtualized async expandable directory tree.
#include "browser/tree_model.hpp"

#include <utility>

namespace navigator::ui {
namespace {

bool is_folder_entry(const fs::DirEntry& e) noexcept {
    return e.kind == fs::EntryKind::Directory ||
           (e.kind == fs::EntryKind::Symlink && e.target_kind == fs::EntryKind::Directory);
}

} // namespace

TreeModel::TreeModel(std::function<void()> wake) : wake_(std::move(wake)) {}

TreeModel::~TreeModel() { stop_worker(); }

void TreeModel::start_worker() {
    if (worker_.joinable() || suspended_) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        stop_ = false;
    }
    worker_ = std::thread([this] {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
                ++active_jobs_;
            }

            fs::ListOptions opts;
            opts.show_hidden = job.show_hidden;
            std::vector<fs::DirEntry> entries;
            if (auto res = fs::list_directory(job.dir, opts); res) {
                entries = std::move(*res);
                fs::sort_entries(entries, job.sort);
            }

            {
                std::lock_guard lock(mutex_);
                done_.push_back(Done{job.generation, std::move(job.dir), std::move(entries)});
                --active_jobs_;
                if (jobs_.empty() && active_jobs_ == 0) {
                    idle_cv_.notify_all();
                }
            }
            if (wake_) {
                wake_();
            }
        }
    });
}

void TreeModel::stop_worker() {
    if (!worker_.joinable()) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    worker_.join();
}

void TreeModel::suspend() {
    if (suspended_) {
        return;
    }
    suspended_ = true;
    stop_worker();
    loading_.clear();
}

void TreeModel::resume() {
    if (!suspended_) {
        return;
    }
    suspended_ = false;
}

void TreeModel::reset() {
    ++generation_;
    expanded_.clear();
    loading_.clear();
    children_.clear();
    rows_.clear();
    {
        std::lock_guard lock(mutex_);
        jobs_.clear();
        done_.clear();
    }
}

void TreeModel::enqueue(const Path& dir, const fs::SortOptions& sort, bool show_hidden) {
    if (suspended_) {
        return;
    }
    start_worker();
    loading_.insert(dir.native());
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(Job{generation_, dir, sort, show_hidden});
    }
    cv_.notify_one();
}

bool TreeModel::expand(const Path& dir, const fs::SortOptions& sort, bool show_hidden) {
    const std::string key = dir.native();
    if (key.empty() || expanded_.contains(key)) {
        return false;
    }
    expanded_.insert(key);
    if (!children_.contains(key) && !loading_.contains(key)) {
        enqueue(dir, sort, show_hidden);
    }
    return true;
}

bool TreeModel::collapse(const Path& dir) {
    return expanded_.erase(dir.native()) > 0;
}

bool TreeModel::toggle(const Path& dir, const fs::SortOptions& sort, bool show_hidden) {
    if (expanded_.contains(dir.native())) {
        return collapse(dir);
    }
    return expand(dir, sort, show_hidden);
}

bool TreeModel::collapse_all() {
    if (expanded_.empty()) {
        return false;
    }
    expanded_.clear();
    return true;
}

bool TreeModel::poll() {
    std::deque<Done> batch;
    {
        std::lock_guard lock(mutex_);
        batch.swap(done_);
    }
    bool changed = false;
    for (Done& item : batch) {
        if (item.generation != generation_) {
            continue;
        }
        const std::string key = item.dir.native();
        loading_.erase(key);
        children_[key] = std::move(item.entries);
        changed = true;
    }
    return changed;
}

void TreeModel::resort_cached(const fs::SortOptions& sort) {
    for (auto& [path, entries] : children_) {
        fs::sort_entries(entries, sort);
    }
}

void TreeModel::reload_expanded(const fs::SortOptions& sort, bool show_hidden) {
    ++generation_;
    children_.clear();
    loading_.clear();
    {
        std::lock_guard lock(mutex_);
        jobs_.clear();
        done_.clear();
    }
    for (const std::string& key : expanded_) {
        enqueue(Path{key}, sort, show_hidden);
    }
}

bool TreeModel::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idle_cv_.wait_for(lock, timeout,
                             [this] { return jobs_.empty() && active_jobs_ == 0; });
}

void TreeModel::append_children(const Path& parent_dir, std::size_t parent_idx,
                                std::uint16_t depth) {
    if (depth >= kMaxTreeExpandDepth) {
        return;
    }
    const auto it = children_.find(parent_dir.native());
    if (it == children_.end()) {
        return;
    }
    for (const fs::DirEntry& child : it->second) {
        const Path child_path = parent_dir / child.name;
        const bool is_dir = is_folder_entry(child);
        const bool is_exp = is_dir && expanded_.contains(child_path.native());
        const bool is_load = is_exp && loading_.contains(child_path.native());
        const std::size_t my_idx = rows_.size();
        rows_.push_back(TreeRow{child_path, child, depth, kTreeNoRootRow, parent_idx, is_dir,
                                is_exp, is_load});
        if (is_exp) {
            append_children(child_path, my_idx, static_cast<std::uint16_t>(depth + 1));
        }
    }
}

void TreeModel::rebuild(const Path& root_dir, std::size_t top_count,
                        const fs::DirEntry* (*entry_at)(const void* ctx, std::size_t row),
                        const void* ctx, const fs::SortOptions& sort, bool show_hidden) {
    rows_.clear();
    if (top_count == 0 || entry_at == nullptr) {
        return;
    }
    rows_.reserve(top_count);
    for (std::size_t r = 0; r < top_count; ++r) {
        const fs::DirEntry* e = entry_at(ctx, r);
        if (e == nullptr) {
            continue;
        }
        const Path full = root_dir / e->name;
        const bool is_dir = is_folder_entry(*e);
        const bool is_exp = is_dir && expanded_.contains(full.native());
        if (is_exp && !children_.contains(full.native()) && !loading_.contains(full.native())) {
            enqueue(full, sort, show_hidden);
        }
        const bool is_load = is_exp && loading_.contains(full.native());
        const std::size_t my_idx = rows_.size();
        rows_.push_back(
            TreeRow{full, *e, 0, r, kTreeNoRootRow, is_dir, is_exp, is_load});
        if (is_exp) {
            append_children(full, my_idx, 1);
        }
    }
}

std::size_t TreeModel::find_path(const Path& full_path) const noexcept {
    const std::string& target = full_path.native();
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].full_path.native() == target) {
            return i;
        }
    }
    return kTreeNoRootRow;
}

} // namespace navigator::ui
