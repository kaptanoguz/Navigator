// src/ui/browser/listing_worker.cpp — see listing_worker.hpp.
#include "browser/listing_worker.hpp"

#include <algorithm>
#include <utility>

namespace navigator::ui {

ListingWorker::ListingWorker(std::function<void()> wake)
    : wake_(std::move(wake)), thread_([this](const std::stop_token& stop) { run(stop); }) {}

ListingWorker::~ListingWorker() {
    {
        const std::scoped_lock lock(mutex_);
        active_.request_stop();
    }
    thread_.request_stop();
    // jthread joins in its destructor; the condition variable wait observes the stop token.
}

void ListingWorker::request(ListingRequest request) {
    {
        const std::scoped_lock lock(mutex_);
        pending_ = std::move(request);
        active_.request_stop(); // a running listing is superseded
    }
    request_cv_.notify_one();
}

std::vector<ListingResult> ListingWorker::take() {
    const std::scoped_lock lock(mutex_);
    std::vector<ListingResult> out;
    out.swap(results_);
    return out;
}

bool ListingWorker::wait_for(std::uint64_t generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return result_cv_.wait_for(lock, timeout, [&] { return newest_posted_ >= generation; });
}

void ListingWorker::run(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        ListingRequest req;
        std::stop_token token;
        {
            std::unique_lock lock(mutex_);
            if (!request_cv_.wait(lock, stop, [this] { return pending_.has_value(); })) {
                return; // stop requested
            }
            if (!pending_) {
                continue;
            }
            req = std::move(*pending_);
            pending_.reset();
            active_ = std::stop_source{};
            token = active_.get_token();
        }
        // Thread shutdown also cancels the listing in progress.
        const std::stop_callback on_shutdown(stop, [this] {
            const std::scoped_lock lock(mutex_);
            active_.request_stop();
        });

        fs::ListOptions options;
        options.show_hidden = true;
        options.follow_symlinks = false;
        ListingResult result;
        result.generation = req.generation;
        result.dir = req.dir;
        result.entries = fs::list_directory(req.dir, options, token);
        if (!result.entries && result.entries.error().code == ErrorCode::Cancelled) {
            continue; // superseded or shutting down: nobody wants this result
        }
        if (result.entries) {
            std::vector<fs::DirEntry>& entries = *result.entries;
            for (const fs::DirEntry& e : entries) {
                if (e.hidden) {
                    result.hidden_names.push_back(e.name);
                }
            }
            if (!req.show_hidden && !result.hidden_names.empty()) {
                std::erase_if(entries, [](const fs::DirEntry& e) { return e.hidden; });
            }
            fs::sort_entries(Span<fs::DirEntry>(entries), req.sort);
        }
        {
            const std::scoped_lock lock(mutex_);
            newest_posted_ = std::max(newest_posted_, result.generation);
            results_.push_back(std::move(result));
        }
        result_cv_.notify_all();
        if (wake_) {
            wake_();
        }
    }
}

} // namespace navigator::ui
