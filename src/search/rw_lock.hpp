// src/search/rw_lock.hpp (internal to navigator_search)
//
// Purpose: writer-preferring shared mutex for the name index. std::shared_mutex on glibc prefers
//          readers, so a steady stream of queries could starve the service thread's short
//          exclusive sections forever; here a waiting writer blocks new readers, so a query waits
//          at most for the readers already inside plus one writer section (index.hpp promise).
// Thread-safety: satisfies Lockable and SharedLockable (usable with std::unique_lock and
//                std::shared_lock); not recursive.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace navigator::search::detail {

class WriterPreferringMutex {
public:
    void lock() {
        std::unique_lock l(m_);
        ++writers_waiting_;
        writer_cv_.wait(l, [&] { return !writer_ && readers_ == 0; });
        --writers_waiting_;
        writer_ = true;
    }
    void unlock() {
        {
            const std::lock_guard l(m_);
            writer_ = false;
        }
        writer_cv_.notify_one();
        reader_cv_.notify_all();
    }
    void lock_shared() {
        std::unique_lock l(m_);
        reader_cv_.wait(l, [&] { return !writer_ && writers_waiting_ == 0; });
        ++readers_;
    }
    void unlock_shared() {
        bool last = false;
        {
            const std::lock_guard l(m_);
            last = --readers_ == 0;
        }
        if (last) {
            writer_cv_.notify_one();
        }
    }

private:
    std::mutex m_;
    std::condition_variable writer_cv_;
    std::condition_variable reader_cv_;
    std::size_t readers_ = 0;
    std::size_t writers_waiting_ = 0;
    bool writer_ = false;
};

} // namespace navigator::search::detail
