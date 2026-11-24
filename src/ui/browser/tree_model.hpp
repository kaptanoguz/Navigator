// src/ui/browser/tree_model.hpp
//
// Phase 9A virtualized async expandable directory tree for BrowserModel.
// Expands folders off-thread (never blocks the UI thread), caches children per expanded path,
// and flattens visible nodes into a linear vector of TreeRow for O(viewport) rendering.
// Thread-safety: public methods are UI-thread only; internal worker runs fs::list_directory on a
// background thread and posts results to a mutex-guarded queue.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

inline constexpr std::size_t kTreeNoRootRow = static_cast<std::size_t>(-1);
inline constexpr std::uint16_t kMaxTreeExpandDepth = 32;

struct TreeRow {
    Path full_path;
    fs::DirEntry entry;
    std::uint16_t depth = 0;
    std::size_t root_row = kTreeNoRootRow; // depth == 0: index in BrowserModel rows
    std::size_t parent_index = kTreeNoRootRow; // index in TreeModel::rows() of parent folder
    bool expandable = false;
    bool expanded = false;
    bool loading = false;
};

class TreeModel {
public:
    explicit TreeModel(std::function<void()> wake = {});
    ~TreeModel();
    TreeModel(const TreeModel&) = delete;
    TreeModel& operator=(const TreeModel&) = delete;
    TreeModel(TreeModel&&) = delete;
    TreeModel& operator=(TreeModel&&) = delete;

    // Resets all expanded folders and cached children (called when navigating to a new folder).
    void reset();

    // Stops the worker thread (for suspended tabs); expanded paths are preserved.
    void suspend();
    void resume();

    // Rebuilds the flattened visible rows from the top-level listing of `root_dir`.
    void rebuild(const Path& root_dir, std::size_t top_count,
                 const fs::DirEntry* (*entry_at)(const void* ctx, std::size_t row), const void* ctx,
                 const fs::SortOptions& sort, bool show_hidden);

    // Expands or collapses `dir`. Expanding starts an async listing if children are not cached.
    // Returns true if the visible tree changed immediately.
    bool expand(const Path& dir, const fs::SortOptions& sort, bool show_hidden);
    bool collapse(const Path& dir);
    bool toggle(const Path& dir, const fs::SortOptions& sort, bool show_hidden);
    bool collapse_all();

    // Applies completed async child listings. Returns true if any cached node updated.
    bool poll();

    // Re-sorts all cached children after sort options change.
    void resort_cached(const fs::SortOptions& sort);

    // Invalidates cached children (e.g., show_hidden toggled or Refresh). Keeps expanded set
    // and re-requests listings for currently expanded folders.
    void reload_expanded(const fs::SortOptions& sort, bool show_hidden);

    // Tests: wait until all pending async listings complete.
    [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);

    [[nodiscard]] const std::vector<TreeRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] bool is_expanded(const Path& dir) const {
        return expanded_.contains(dir.native());
    }
    [[nodiscard]] bool busy() const noexcept { return !loading_.empty(); }

    // Finds the TreeRow index matching `full_path`, or kTreeNoRootRow.
    [[nodiscard]] std::size_t find_path(const Path& full_path) const noexcept;

private:
    struct Job {
        std::uint64_t generation = 0;
        Path dir;
        fs::SortOptions sort;
        bool show_hidden = false;
    };
    struct Done {
        std::uint64_t generation = 0;
        Path dir;
        std::vector<fs::DirEntry> entries;
    };

    void start_worker();
    void stop_worker();
    void enqueue(const Path& dir, const fs::SortOptions& sort, bool show_hidden);
    void append_children(const Path& parent_dir, std::size_t parent_idx, std::uint16_t depth);

    std::function<void()> wake_;
    std::uint64_t generation_ = 1;
    bool suspended_ = false;

    std::unordered_set<std::string> expanded_;
    std::unordered_set<std::string> loading_;
    std::unordered_map<std::string, std::vector<fs::DirEntry>> children_;
    std::vector<TreeRow> rows_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::deque<Job> jobs_;
    std::deque<Done> done_;
    std::size_t active_jobs_ = 0;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace navigator::ui
