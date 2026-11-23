// Helpers of the Phase 5 ui tests: a scripted walk source, a fake index and settle loops.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/index.hpp>
#include <navigator/walk.hpp>

#include "browser/browser_model.hpp"

namespace navigator::test {

inline constexpr std::chrono::milliseconds kSearchWait{5000};

inline fs::DirEntry make_entry(std::string name, bool dir = false) {
    fs::DirEntry e;
    e.name = std::move(name);
    e.kind = dir ? fs::EntryKind::Directory : fs::EntryKind::File;
    e.hidden = fs::is_hidden_name(e.name);
    return e;
}

// Pumps frame_start() until `done` (or the timeout).
inline bool pump_until(ui::BrowserModel& m, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + kSearchWait;
    while (true) {
        (void)m.frame_start();
        if (done()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

inline void open_listed(ui::BrowserModel& m, const Path& dir) {
    m.open_start(dir);
    REQUIRE(m.wait_listing(kSearchWait));
    (void)m.frame_start();
    REQUIRE_FALSE(m.busy());
    REQUIRE(m.settle_search(kSearchWait));
}

inline std::vector<std::string> rows_of(const ui::BrowserModel& m) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < m.row_count(); ++i) {
        out.push_back(m.row_entry(i).name);
    }
    return out;
}

inline std::string cursor_of(const ui::BrowserModel& m) {
    const fs::DirEntry* e = m.cursor_entry();
    return e != nullptr ? e->name : std::string();
}

// A walk the test feeds batch by batch; next_batch() blocks until fed, ended or cancelled.
class WalkScript {
public:
    void feed(fs::WalkBatch batch) {
        {
            const std::scoped_lock lock(mutex_);
            batches_.push_back(std::move(batch));
        }
        cv_.notify_all();
    }
    void end() {
        {
            const std::scoped_lock lock(mutex_);
            ended_ = true;
        }
        cv_.notify_all();
    }
    void fail_open(ErrorCode code) { open_error_ = code; }
    [[nodiscard]] int opened() const { return opened_.load(); }
    [[nodiscard]] int cancelled() const { return cancelled_.load(); }
    [[nodiscard]] fs::WalkOptions options() {
        const std::scoped_lock lock(mutex_);
        return options_;
    }

    [[nodiscard]] static ui::WalkFactory factory(const std::shared_ptr<WalkScript>& script) {
        return [script](const fs::WalkOptions& o) -> Result<std::unique_ptr<ui::WalkSource>> {
            {
                const std::scoped_lock lock(script->mutex_);
                script->options_ = o;
            }
            script->opened_.fetch_add(1);
            if (script->open_error_) {
                return fail(*script->open_error_, "scripted");
            }
            return std::unique_ptr<ui::WalkSource>(std::make_unique<Source>(script));
        };
    }

private:
    class Source final : public ui::WalkSource {
    public:
        explicit Source(std::shared_ptr<WalkScript> script) : s_(std::move(script)) {}
        Result<bool> next_batch(fs::WalkBatch& out, std::stop_token stop) override {
            std::unique_lock lock(s_->mutex_);
            const bool ready =
                s_->cv_.wait(lock, stop, [this] { return !s_->batches_.empty() || s_->ended_; });
            if (!ready || stop.stop_requested()) {
                s_->cancelled_.fetch_add(1);
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            out.clear();
            if (s_->batches_.empty()) {
                stats_.done = true;
                return false;
            }
            out = std::move(s_->batches_.front());
            s_->batches_.pop_front();
            stats_.entries_returned += out.entries.size();
            ++stats_.dirs_visited;
            return true;
        }
        [[nodiscard]] const fs::WalkStats& stats() const noexcept override { return stats_; }

    private:
        std::shared_ptr<WalkScript> s_;
        fs::WalkStats stats_;
    };

    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<fs::WalkBatch> batches_;
    bool ended_ = false;
    std::optional<ErrorCode> open_error_;
    fs::WalkOptions options_;
    std::atomic<int> opened_{0};
    std::atomic<int> cancelled_{0};
};

class FakeIndex final : public search::IIndex {
public:
    explicit FakeIndex(Path root) : root_(std::move(root)) {}

    void start() override { starts.fetch_add(1); }
    void stop() noexcept override { stops.fetch_add(1); }
    void request_rescan(search::RescanKind kind) override {
        if (kind == search::RescanKind::Rebuild) {
            rebuilds.fetch_add(1);
        }
    }
    [[nodiscard]] search::IndexStatus status() const override {
        search::IndexStatus s;
        s.state = search::IndexState::Ready;
        s.generation = generation.load();
        return s;
    }
    [[nodiscard]] const Path& root() const noexcept override { return root_; }
    [[nodiscard]] Result<search::IndexResult> query(const search::IndexQuery& query,
                                                    std::stop_token /*stop*/) const override {
        search::IndexResult r;
        search::Matcher matcher(query.query);
        const std::scoped_lock lock(mutex_);
        for (const std::string& rel : names_) {
            const Path p = root_ / rel;
            if (auto m = matcher.match(p.filename().string()); m) {
                search::IndexHit hit;
                hit.path = p;
                hit.kind = fs::EntryKind::File;
                hit.match = std::move(*m);
                hit.depth = 1;
                for (const char c : rel) {
                    hit.depth += c == '/' ? 1U : 0U;
                }
                r.hits.push_back(std::move(hit));
            }
        }
        r.matched = r.hits.size();
        r.complete = true;
        r.generation = generation.load();
        return r;
    }
    void set_names(std::vector<std::string> names) {
        {
            const std::scoped_lock lock(mutex_);
            names_ = std::move(names);
        }
        generation.fetch_add(1);
    }

    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<int> rebuilds{0};
    std::atomic<std::uint64_t> generation{0};

private:
    Path root_;
    mutable std::mutex mutex_;
    std::vector<std::string> names_;
};

} // namespace navigator::test
