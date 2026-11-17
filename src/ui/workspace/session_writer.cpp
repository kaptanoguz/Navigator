// src/ui/workspace/session_writer.cpp — see session_writer.hpp.
#include "workspace/session_writer.hpp"

#include <utility>

namespace navigator::ui {

SessionWriter::SessionWriter(Path file, Log log)
    : file_(std::move(file)), log_(std::move(log)),
      thread_([this](const std::stop_token& stop) { run(stop); }) {}

SessionWriter::~SessionWriter() {
    (void)flush();
    thread_.request_stop();
    cv_.notify_all();
}

void SessionWriter::submit(layout::Session session) {
    {
        const std::scoped_lock lock(mutex_);
        mailbox_ = std::move(session); // latest wins
    }
    cv_.notify_one();
}

bool SessionWriter::flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idle_cv_.wait_for(lock, timeout, [this] { return !mailbox_ && !writing_; });
}

Result<void> SessionWriter::save_now(const layout::Session& session) {
    Result<void> r = layout::save_session(file_, session);
    if (!r && log_) {
        log_("session save failed: " + r.error().message);
    }
    if (r) {
        const std::scoped_lock lock(mutex_);
        ++writes_;
    }
    return r;
}

std::uint64_t SessionWriter::writes() const {
    const std::scoped_lock lock(mutex_);
    return writes_;
}

void SessionWriter::run(const std::stop_token& stop) {
    while (true) {
        std::optional<layout::Session> job;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait(lock, stop, [this] { return mailbox_.has_value(); })) {
                return;
            }
            job.swap(mailbox_); // latest wins; the mailbox is empty again
            writing_ = job.has_value();
        }
        if (!job) {
            continue;
        }
        const Result<void> r = layout::save_session(file_, *job);
        if (!r && log_) {
            log_("session save failed: " + r.error().message);
        }
        {
            const std::scoped_lock lock(mutex_);
            writing_ = false;
            if (r) {
                ++writes_;
            }
        }
        idle_cv_.notify_all();
    }
}

// ---------------------------------------------------------------------------------------------
// JobWriter
// ---------------------------------------------------------------------------------------------

JobWriter::JobWriter() : thread_([this](const std::stop_token& stop) { run(stop); }) {}

JobWriter::~JobWriter() {
    (void)flush();
    thread_.request_stop();
    cv_.notify_all();
}

void JobWriter::submit(Job job) {
    {
        const std::scoped_lock lock(mutex_);
        mailbox_ = std::move(job);
    }
    cv_.notify_one();
}

bool JobWriter::flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idle_cv_.wait_for(lock, timeout, [this] { return !mailbox_ && !writing_; });
}

std::uint64_t JobWriter::writes() const {
    const std::scoped_lock lock(mutex_);
    return writes_;
}

void JobWriter::run(const std::stop_token& stop) {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait(lock, stop, [this] { return static_cast<bool>(mailbox_); })) {
                return;
            }
            job.swap(mailbox_);
            writing_ = static_cast<bool>(job);
        }
        if (!job) {
            continue;
        }
        const Result<void> r = job();
        {
            const std::scoped_lock lock(mutex_);
            writing_ = false;
            if (r) {
                ++writes_;
            }
        }
        idle_cv_.notify_all();
    }
}

} // namespace navigator::ui
