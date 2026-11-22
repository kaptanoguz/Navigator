// src/ui/search/search_worker.hpp
//
// Phase 5: the per-tab worker for CPU-bound search jobs that must not run on the UI thread
// (docs/architecture.md "Phase 5", "Data flow"): the folder filter of listings above
// search::kUiThreadFilterLimit, organizing (matching + ordering) the rows of a flattened view,
// and name-index queries. One std::jthread; jobs are closures that post their own results into a
// ResultBox and call the wake callback.
//
// Lanes: each lane holds at most one queued job (a newer post replaces it, "latest wins"). A
// post with `supersede` also asks a RUNNING job of the same lane to stop (its stop token), so a
// changed query never waits for a stale one. Jobs check their token at least every
// search::kFilterStopCheckInterval entries, which bounds the join in the destructor (pure CPU
// work, never blocking I/O: only index queries stat their few hundred hits, on local volumes).
// Thread-safety: post()/idle() from the owning (UI) thread; jobs run on the worker thread.
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace navigator::ui {

// A thread-safe mailbox the jobs post into and the UI thread drains at frame start.
template <class T> class ResultBox {
public:
    void push(T value) {
        const std::scoped_lock lock(mutex_);
        items_.push_back(std::move(value));
    }
    [[nodiscard]] std::vector<T> take() {
        const std::scoped_lock lock(mutex_);
        std::vector<T> out;
        out.swap(items_);
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<T> items_;
};

class SearchWorker {
public:
    using Job = std::function<void(const std::stop_token&)>;
    static constexpr std::size_t kLanes = 2;
    static constexpr std::size_t kListingLane = 0;
    static constexpr std::size_t kFlatLane = 1;

    SearchWorker();
    ~SearchWorker();
    SearchWorker(const SearchWorker&) = delete;
    SearchWorker& operator=(const SearchWorker&) = delete;
    SearchWorker(SearchWorker&&) = delete;
    SearchWorker& operator=(SearchWorker&&) = delete;

    void post(std::size_t lane, Job job, bool supersede);
    // Drops the queued job of `lane` and stops a running one.
    void cancel(std::size_t lane);
    // No job queued or running (tests and scripted runs).
    [[nodiscard]] bool idle() const;
    [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout) const;

private:
    void run(const std::stop_token& stop);

    mutable std::mutex mutex_;
    mutable std::condition_variable_any cv_;
    mutable std::condition_variable idle_cv_;
    std::array<std::optional<Job>, kLanes> queued_;
    std::optional<std::size_t> running_lane_;
    std::stop_source running_stop_;
    std::jthread thread_; // last: starts after the members above exist
};

} // namespace navigator::ui
