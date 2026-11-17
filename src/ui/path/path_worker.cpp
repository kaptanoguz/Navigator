// src/ui/path/path_worker.cpp — see path_worker.hpp.
#include "path/path_worker.hpp"

#include <utility>

namespace navigator::ui {

CompletionReply run_completion(const CompletionRequest& request, const std::stop_token& stop) {
    CompletionReply reply;
    reply.id = request.id;
    Result<path_input::CompletionQuery> q =
        path_input::parse_completion_input(request.input, request.context);
    if (!q) {
        return reply;
    }
    reply.ok = true;
    reply.query = std::move(*q);
    fs::ListOptions lo;
    lo.show_hidden = true;
    Result<std::vector<fs::DirEntry>> listed = fs::list_directory(reply.query.dir, lo, stop);
    if (!listed && listed.error().code != ErrorCode::Cancelled) {
        // "~/dow/" -> "~/Downloads/": correct the case of the folder being completed.
        Result<path_input::CaseFix> fix = path_input::fix_path_case(reply.query.dir, stop);
        if (fix && fix->changed && fix->exists) {
            reply.query.dir = fix->path;
            const std::string s = fix->path.string();
            reply.query.typed_dir = s == "/" ? s : s + "/";
            listed = fs::list_directory(reply.query.dir, lo, stop);
        }
    }
    if (listed) {
        reply.entries = std::move(*listed);
    }
    return reply;
}

PathWorker::PathWorker(std::function<void()> wake)
    : wake_(std::move(wake)), thread_([this](const std::stop_token& stop) { run(stop); }) {}

PathWorker::~PathWorker() {
    {
        const std::scoped_lock lock(mutex_);
        running_completion_.request_stop();
    }
    thread_.request_stop();
    cv_.notify_all();
}

void PathWorker::complete(CompletionRequest request) {
    {
        const std::scoped_lock lock(mutex_);
        completion_job_ = std::move(request);
        running_completion_.request_stop(); // a running listing for an older input
    }
    cv_.notify_all();
}

void PathWorker::resolve(ResolveRequest request) {
    {
        const std::scoped_lock lock(mutex_);
        resolve_job_ = std::move(request);
    }
    cv_.notify_all();
}

std::optional<CompletionReply> PathWorker::take_completion() {
    const std::scoped_lock lock(mutex_);
    std::optional<CompletionReply> out;
    out.swap(completion_reply_);
    return out;
}

std::optional<ResolveReply> PathWorker::take_resolve() {
    const std::scoped_lock lock(mutex_);
    std::optional<ResolveReply> out;
    out.swap(resolve_reply_);
    return out;
}

bool PathWorker::busy() const {
    const std::scoped_lock lock(mutex_);
    return running_ || completion_job_.has_value() || resolve_job_.has_value() ||
           completion_reply_.has_value() || resolve_reply_.has_value();
}

bool PathWorker::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] {
        return !running_ && !completion_job_.has_value() && !resolve_job_.has_value();
    });
}

void PathWorker::run(const std::stop_token& stop) {
    while (true) {
        std::optional<ResolveRequest> resolve;
        std::optional<CompletionRequest> complete;
        std::stop_token job_stop;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait(lock, stop, [this] {
                    return completion_job_.has_value() || resolve_job_.has_value();
                })) {
                return;
            }
            if (resolve_job_) {
                resolve.swap(resolve_job_);
            } else {
                complete.swap(completion_job_);
                running_completion_ = std::stop_source();
                job_stop = running_completion_.get_token();
            }
            running_ = true;
        }
        if (resolve) {
            ResolveReply reply;
            reply.id = resolve->id;
            reply.result = path_input::resolve_input(resolve->input, resolve->context, stop);
            const std::scoped_lock lock(mutex_);
            resolve_reply_ = std::move(reply);
        } else if (complete) {
            CompletionReply reply = run_completion(*complete, job_stop);
            const std::scoped_lock lock(mutex_);
            if (!job_stop.stop_requested()) {
                completion_reply_ = std::move(reply);
            }
        }
        {
            const std::scoped_lock lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
        if (wake_) {
            wake_();
        }
    }
}

} // namespace navigator::ui
