// src/ui/path/path_worker.hpp
//
// Background jobs of the GoTo box and the address bar (docs/architecture.md "GoTo and the
// address bar"): directory listings for folder-name completion and the Enter resolution
// (path_input::resolve_input: expand, case-correct, stat). Both block on I/O (and "~user" on
// NSS), so they never run on the UI thread.
//   - One std::jthread, two latest-wins mailboxes (a newer request replaces a queued one and
//     cancels a running one of the same kind through its std::stop_source).
//   - Completion: parse_completion_input with the full context; list query.dir (folders and
//     links to folders are filtered later); when the folder does not exist, fix_path_case() on
//     it and, when that finds it, the typed directory becomes the corrected absolute path.
//     The UI caches the listing and runs match_completions() per keystroke.
//   - Replies carry the request id; the UI drops stale ones.
// Thread-safety: all members may be called from the UI thread while the worker runs.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

struct CompletionRequest {
    std::uint64_t id = 0;
    std::string input;
    path_input::ExpandContext context;
};

struct CompletionReply {
    std::uint64_t id = 0;
    bool ok = false;                   // the directory part could be parsed
    path_input::CompletionQuery query; // dir / typed_dir possibly case-corrected
    std::vector<fs::DirEntry> entries; // listing of query.dir (empty when it cannot be listed)
};

struct ResolveRequest {
    std::uint64_t id = 0;
    std::string input;
    path_input::ExpandContext context;
};

struct ResolveReply {
    std::uint64_t id = 0;
    Result<path_input::ResolvedInput> result = fail(ErrorCode::Unknown, "not run");
};

// The worker side of a completion request (blocking; exposed for tests).
[[nodiscard]] CompletionReply run_completion(const CompletionRequest& request,
                                             const std::stop_token& stop = {});

class PathWorker {
public:
    explicit PathWorker(std::function<void()> wake = {});
    ~PathWorker();
    PathWorker(const PathWorker&) = delete;
    PathWorker& operator=(const PathWorker&) = delete;
    PathWorker(PathWorker&&) = delete;
    PathWorker& operator=(PathWorker&&) = delete;

    void complete(CompletionRequest request);
    void resolve(ResolveRequest request);
    [[nodiscard]] std::optional<CompletionReply> take_completion();
    [[nodiscard]] std::optional<ResolveReply> take_resolve();
    // A request is queued or running, or a reply is waiting.
    [[nodiscard]] bool busy() const;
    // Tests and scripted runs: blocks until nothing is queued or running.
    [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);

private:
    void run(const std::stop_token& stop);

    std::function<void()> wake_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::optional<CompletionRequest> completion_job_;
    std::optional<ResolveRequest> resolve_job_;
    std::optional<CompletionReply> completion_reply_;
    std::optional<ResolveReply> resolve_reply_;
    std::stop_source running_completion_;
    bool running_ = false;
    std::jthread thread_; // last member
};

} // namespace navigator::ui
