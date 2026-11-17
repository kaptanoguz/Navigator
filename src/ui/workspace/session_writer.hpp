// src/ui/workspace/session_writer.hpp
//
// Session persistence timing (docs/architecture.md, "Persistence"):
//   - SaveScheduler: pure debounce. mark() on every persisted change; due() turns true
//     kDebounce after the LAST mark; taken() resets it.
//   - SessionWriter: background writer, one std::jthread with a latest-wins mailbox. submit()
//     hands over a Session copy and returns at once; the thread calls layout::save_session().
//     flush() waits until nothing is queued or being written (save on quit runs flush() and then
//     writes synchronously with save_now()). Failures are logged through `log`, never thrown.
// Thread-safety: SaveScheduler is a UI-thread value. SessionWriter's members may be called from
// any thread (the UI thread in practice); `log` runs on the writer thread or the caller.
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

#include <navigator/layout.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

class SaveScheduler {
public:
    static constexpr double kDebounceMs = 2000.0;

    void mark(double now_ms) noexcept { deadline_ = now_ms + kDebounceMs; }
    [[nodiscard]] bool pending() const noexcept { return deadline_.has_value(); }
    [[nodiscard]] bool due(double now_ms) const noexcept {
        return deadline_.has_value() && now_ms >= *deadline_;
    }
    // Milliseconds until due (0 when due), or nullopt when nothing is pending.
    [[nodiscard]] std::optional<double> remaining(double now_ms) const noexcept {
        if (!deadline_) {
            return std::nullopt;
        }
        return *deadline_ > now_ms ? *deadline_ - now_ms : 0.0;
    }
    void taken() noexcept { deadline_.reset(); }

private:
    std::optional<double> deadline_;
};

class SessionWriter {
public:
    using Log = std::function<void(const std::string&)>;

    SessionWriter(Path file, Log log = {});
    ~SessionWriter(); // finishes the write in progress; a queued one is still written
    SessionWriter(const SessionWriter&) = delete;
    SessionWriter& operator=(const SessionWriter&) = delete;
    SessionWriter(SessionWriter&&) = delete;
    SessionWriter& operator=(SessionWriter&&) = delete;

    void submit(layout::Session session);
    // Blocks until the mailbox is empty and no write is running, or `timeout`.
    bool flush(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    // Synchronous write on the calling thread (after flush()).
    Result<void> save_now(const layout::Session& session);

    [[nodiscard]] std::uint64_t writes() const;
    [[nodiscard]] const Path& file() const noexcept { return file_; }

private:
    void run(const std::stop_token& stop);

    Path file_;
    Log log_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::condition_variable idle_cv_;
    std::optional<layout::Session> mailbox_;
    bool writing_ = false;
    std::uint64_t writes_ = 0;
    std::jthread thread_; // last member
};

// Phase 4: the same latest-wins background writer for any small file (bookmarks.json,
// recents.json). A job is a closure that owns a copy of what it writes; submit() replaces a
// queued job that has not started. flush() waits until idle. Failures are logged by the job.
class JobWriter {
public:
    using Job = std::function<Result<void>()>;

    JobWriter();
    ~JobWriter(); // finishes the write in progress; a queued job is still run
    JobWriter(const JobWriter&) = delete;
    JobWriter& operator=(const JobWriter&) = delete;
    JobWriter(JobWriter&&) = delete;
    JobWriter& operator=(JobWriter&&) = delete;

    void submit(Job job);
    bool flush(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
    [[nodiscard]] std::uint64_t writes() const;

private:
    void run(const std::stop_token& stop);

    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::condition_variable idle_cv_;
    Job mailbox_;
    bool writing_ = false;
    std::uint64_t writes_ = 0;
    std::jthread thread_; // last member
};

} // namespace navigator::ui
