// src/ui/search/search_worker.cpp — see search_worker.hpp.
#include "search/search_worker.hpp"

namespace navigator::ui {

SearchWorker::SearchWorker() : thread_([this](const std::stop_token& stop) { run(stop); }) {}

SearchWorker::~SearchWorker() {
    {
        const std::scoped_lock lock(mutex_);
        for (std::optional<Job>& q : queued_) {
            q.reset();
        }
        running_stop_.request_stop();
    }
    thread_.request_stop();
    cv_.notify_all();
    // jthread joins: the running job observes its stop token within one check interval.
}

void SearchWorker::post(std::size_t lane, Job job, bool supersede) {
    if (lane >= kLanes) {
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        queued_[lane] = std::move(job);
        if (supersede && running_lane_ == lane) {
            running_stop_.request_stop();
        }
    }
    cv_.notify_all();
}

void SearchWorker::cancel(std::size_t lane) {
    if (lane >= kLanes) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    queued_[lane].reset();
    if (running_lane_ == lane) {
        running_stop_.request_stop();
    }
}

bool SearchWorker::idle() const {
    const std::scoped_lock lock(mutex_);
    if (running_lane_) {
        return false;
    }
    for (const std::optional<Job>& q : queued_) {
        if (q) {
            return false;
        }
    }
    return true;
}

bool SearchWorker::wait_idle(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return idle_cv_.wait_for(lock, timeout, [this] {
        if (running_lane_) {
            return false;
        }
        for (const std::optional<Job>& q : queued_) {
            if (q) {
                return false;
            }
        }
        return true;
    });
}

void SearchWorker::run(const std::stop_token& stop) {
    while (true) {
        Job job;
        std::stop_token token;
        {
            std::unique_lock lock(mutex_);
            const auto has_job = [this] {
                for (const std::optional<Job>& q : queued_) {
                    if (q) {
                        return true;
                    }
                }
                return false;
            };
            if (!cv_.wait(lock, stop, has_job)) {
                return;
            }
            // Lane 0 (listing filter, interactive) before lane 1 (flattened rows).
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                if (std::optional<Job>& next = queued_[lane]; next.has_value()) {
                    job = std::move(*next);
                    next.reset();
                    running_lane_ = lane;
                    break;
                }
            }
            running_stop_ = std::stop_source{};
            token = running_stop_.get_token();
        }
        {
            const std::stop_callback on_shutdown(stop, [this] {
                const std::scoped_lock lock(mutex_);
                running_stop_.request_stop();
            });
            if (job) {
                job(token);
            }
        }
        {
            const std::scoped_lock lock(mutex_);
            running_lane_.reset();
        }
        idle_cv_.notify_all();
    }
}

} // namespace navigator::ui
