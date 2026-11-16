// tests/integration/browser_support.hpp: helpers for the BrowserModel integration tests (owner:
// qa). Headless: links navigator_ui_browser only (no SDL, no window).
//
// - Watcher decorators wrap the real inotify backend: LossyWatcher drops whole poll() results
//   and reports Overflow in their place (what the kernel does when its queue fills); GatedWatcher
//   holds add() until the test opens the gate (makes the "listing finished before the watch was
//   added" window deterministic).
// - NoSpawnLauncher records requests and never starts a process.
// - Invariant and fresh-listing checks run on the test thread only (Catch2 is not thread-safe).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/platform.hpp>
#include <navigator/types.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"

namespace navigator::integration_test {

inline constexpr std::chrono::milliseconds kBrowserWait{5000};

// Never spawns: records what the browser asked to launch.
class NoSpawnLauncher final : public platform::IAppLauncher {
public:
    Result<void> launch(const platform::LaunchRequest& request) override {
        requests.push_back(request);
        return ok();
    }
    std::vector<platform::LaunchRequest> requests;
};

// Replaces a whole poll() result with one Overflow event, with probability `drop_percent` / 100.
// Used by one worker thread at a time, like the watcher it wraps.
class LossyWatcher final : public IWatcher {
public:
    LossyWatcher(std::unique_ptr<IWatcher> inner, unsigned drop_percent, std::uint32_t seed,
                 std::shared_ptr<std::atomic<int>> overflows)
        : inner_(std::move(inner)), drop_percent_(drop_percent), rng_(seed),
          overflows_(std::move(overflows)) {}

    Result<WatchHandle> add(const Path& dir) override { return inner_->add(dir); }
    void remove(WatchHandle handle) noexcept override { inner_->remove(handle); }
    Result<std::size_t> poll(std::vector<WatchEvent>& out) override {
        std::vector<WatchEvent> got;
        Result<std::size_t> n = inner_->poll(got);
        if (!n) {
            return n;
        }
        if (!got.empty() && std::uniform_int_distribution<unsigned>(0, 99)(rng_) < drop_percent_) {
            WatchEvent overflow;
            overflow.kind = WatchEventKind::Overflow;
            out.push_back(std::move(overflow));
            overflows_->fetch_add(1, std::memory_order_relaxed);
            return std::size_t{1};
        }
        out.insert(out.end(), std::make_move_iterator(got.begin()),
                   std::make_move_iterator(got.end()));
        return got.size();
    }
    bool wait(std::chrono::milliseconds timeout) override { return inner_->wait(timeout); }
    [[nodiscard]] int native_handle() const noexcept override { return inner_->native_handle(); }

private:
    std::unique_ptr<IWatcher> inner_;
    unsigned drop_percent_;
    std::mt19937 rng_;
    std::shared_ptr<std::atomic<int>> overflows_;
};

// Shared gate for GatedWatcher::add(). Open by default.
struct WatchGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = true;
    int blocked = 0; // add() calls currently waiting

    void close() {
        const std::scoped_lock lock(mutex);
        open = false;
    }
    void release() {
        {
            const std::scoped_lock lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
    // Test thread: waits until an add() is held at the gate.
    bool wait_blocked(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [this] { return blocked > 0; });
    }
};

class GatedWatcher final : public IWatcher {
public:
    GatedWatcher(std::unique_ptr<IWatcher> inner, std::shared_ptr<WatchGate> gate)
        : inner_(std::move(inner)), gate_(std::move(gate)) {}

    Result<WatchHandle> add(const Path& dir) override {
        {
            std::unique_lock lock(gate_->mutex);
            ++gate_->blocked;
            gate_->cv.notify_all();
            gate_->cv.wait(lock, [this] { return gate_->open; });
            --gate_->blocked;
        }
        return inner_->add(dir);
    }
    void remove(WatchHandle handle) noexcept override { inner_->remove(handle); }
    Result<std::size_t> poll(std::vector<WatchEvent>& out) override { return inner_->poll(out); }
    bool wait(std::chrono::milliseconds timeout) override { return inner_->wait(timeout); }
    [[nodiscard]] int native_handle() const noexcept override { return inner_->native_handle(); }

private:
    std::unique_ptr<IWatcher> inner_;
    std::shared_ptr<WatchGate> gate_;
};

[[nodiscard]] inline std::vector<std::string> entry_names(const ui::BrowserModel& m) {
    std::vector<std::string> out;
    out.reserve(m.entries().size());
    for (const fs::DirEntry& e : m.entries()) {
        out.push_back(e.name);
    }
    return out;
}

[[nodiscard]] inline std::set<std::string> selected_set(const ui::BrowserModel& m) {
    std::set<std::string> out;
    for (std::size_t i = 0; i < m.entries().size(); ++i) {
        if (m.is_selected(i)) {
            out.insert(m.entries()[i].name);
        }
    }
    return out;
}

[[nodiscard]] inline std::string cursor_entry(const ui::BrowserModel& m) {
    return m.cursor() < m.entries().size() ? m.entries()[m.cursor()].name : std::string();
}

// Structural invariants that must hold after every frame_start() and every command (the order
// and hidden checks only while no listing is pending: sort_by() and toggle_hidden() switch sort()
// and show_hidden() before the re-list lands). Returns an
// empty string when they hold, else a description of the first violation.
[[nodiscard]] inline std::string invariant_violation(const ui::BrowserModel& m) {
    const std::vector<fs::DirEntry>& entries = m.entries();
    if (m.cursor() != ui::kNoRow && m.cursor() >= entries.size()) {
        return "cursor out of range";
    }
    if (m.anchor() != ui::kNoRow && m.anchor() >= entries.size()) {
        return "anchor out of range";
    }
    if (entries.empty() && m.cursor() != ui::kNoRow) {
        return "cursor set on an empty list";
    }
    std::size_t selected = 0;
    std::uint64_t bytes = 0;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!seen.insert(entries[i].name).second) {
            return "duplicate entry '" + entries[i].name + "'";
        }
        if (!m.busy() && !m.show_hidden() && fs::is_hidden_name(entries[i].name)) {
            return "hidden entry '" + entries[i].name + "' listed while hidden items are off";
        }
        if (m.is_selected(i)) {
            ++selected;
            if (!fs::is_directory_like(entries[i])) {
                bytes += entries[i].size;
            }
        }
    }
    if (m.is_selected(entries.size())) {
        return "selection flag past the end";
    }
    if (selected != m.selected_count()) {
        return "selected_count " + std::to_string(m.selected_count()) + " != flags " +
               std::to_string(selected);
    }
    if (bytes != m.selected_bytes()) {
        return "selected_bytes mismatch";
    }
    if (m.busy()) {
        return {}; // a re-sort is in flight: the old order stays visible until it commits
    }
    std::vector<fs::DirEntry> sorted = entries;
    fs::sort_entries(Span<fs::DirEntry>(sorted), m.sort());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (sorted[i].name != entries[i].name) {
            return "not sorted at row " + std::to_string(i) + ": '" + entries[i].name +
                   "' where sort_entries puts '" + sorted[i].name + "'";
        }
    }
    return {};
}

// What a fresh listing of the model's directory shows: visible names (with kind and size for
// comparison) and the hidden count. `ok` is false when the directory cannot be listed.
struct FreshView {
    bool ok = false;
    std::vector<std::string> rows; // "name|d" for folders, "name|f|<size>" otherwise
    std::size_t hidden = 0;
};

[[nodiscard]] inline std::string row_key(const fs::DirEntry& e) {
    return fs::is_directory_like(e) ? e.name + "|d" : e.name + "|f|" + std::to_string(e.size);
}

[[nodiscard]] inline FreshView fresh_view(const Path& dir, bool show_hidden) {
    FreshView v;
    fs::ListOptions options;
    options.show_hidden = true;
    options.follow_symlinks = false;
    Result<std::vector<fs::DirEntry>> listed = fs::list_directory(dir, options);
    if (!listed) {
        return v;
    }
    v.ok = true;
    for (const fs::DirEntry& e : *listed) {
        if (e.hidden) {
            ++v.hidden;
            if (!show_hidden) {
                continue;
            }
        }
        v.rows.push_back(row_key(e));
    }
    std::sort(v.rows.begin(), v.rows.end());
    return v;
}

[[nodiscard]] inline std::vector<std::string> model_rows(const ui::BrowserModel& m) {
    std::vector<std::string> rows;
    rows.reserve(m.entries().size());
    for (const fs::DirEntry& e : m.entries()) {
        rows.push_back(row_key(e));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

// Lets pending listings and watch batches drain: returns once nothing is pending and no watch
// batch arrived for `quiet`, or after `limit`. Returns false on timeout.
inline bool quiesce(ui::BrowserModel& m,
                    std::chrono::milliseconds quiet = std::chrono::milliseconds(400),
                    std::chrono::milliseconds limit = std::chrono::milliseconds(15000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)m.wait_listing(kBrowserWait);
        (void)m.frame_start();
        if (m.busy()) {
            continue;
        }
        if (!m.wait_watch_batch(quiet)) {
            (void)m.frame_start();
            if (!m.busy()) {
                return true;
            }
        }
    }
    return false;
}

} // namespace navigator::integration_test
