// src/ui/browser/listing_worker.hpp
//
// Off-UI-thread directory listing for the browser. One std::jthread runs fs::list_directory()
// (cancellable through a std::stop_token) and fs::sort_entries(), then posts the result to a
// mailbox and calls `wake` so the UI loop applies it at the next frame start.
//
// Only the newest request matters: request() replaces a request that has not started and asks a
// running listing to stop. Results carry the request's generation; the consumer drops stale ones.
// Hidden entries: the worker always lists with show_hidden = true, counts the hidden names and
// removes them when the request does not show hidden items, so the status bar and the
// "only hidden items" empty state know how many there are.
//
// Thread-safety: request(), take() and wait_for() may be called from any thread (the UI thread in
// practice); `wake` is called on the worker thread and must be thread-safe (SDL_PushEvent is).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

struct ListingRequest {
    std::uint64_t generation = 0;
    Path dir;
    bool show_hidden = false;
    fs::SortOptions sort;
};

struct ListingResult {
    std::uint64_t generation = 0;
    Path dir;
    Result<std::vector<fs::DirEntry>> entries = std::vector<fs::DirEntry>{};
    std::vector<std::string> hidden_names; // hidden names in the directory (listed or not)
};

class ListingWorker {
public:
    explicit ListingWorker(std::function<void()> wake = {});
    ~ListingWorker();
    ListingWorker(const ListingWorker&) = delete;
    ListingWorker& operator=(const ListingWorker&) = delete;
    ListingWorker(ListingWorker&&) = delete;
    ListingWorker& operator=(ListingWorker&&) = delete;

    void request(ListingRequest request);

    // Finished results in completion order (cancelled listings are never posted).
    [[nodiscard]] std::vector<ListingResult> take();

    // Blocks until a result with generation >= `generation` is in the mailbox, or `timeout`.
    [[nodiscard]] bool wait_for(std::uint64_t generation, std::chrono::milliseconds timeout);

private:
    void run(const std::stop_token& stop);

    std::function<void()> wake_;
    std::mutex mutex_;
    std::condition_variable_any request_cv_;
    std::condition_variable result_cv_;
    std::optional<ListingRequest> pending_;
    std::stop_source active_; // stop source of the listing that is running (guarded by mutex_)
    std::vector<ListingResult> results_;
    std::uint64_t newest_posted_ = 0;
    std::jthread thread_; // last member: started after everything above is constructed
};

} // namespace navigator::ui
