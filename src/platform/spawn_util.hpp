// Internal (src/platform): fd helpers shared by the launcher and the process runner. Everything
// marked async-signal-safe may run between fork() and exec().
#pragma once

#include <climits>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace navigator::platform_internal {

// Owns one file descriptor; closes it on destruction or reset().
class FdGuard {
public:
    explicit FdGuard(int fd = -1) noexcept : fd_(fd) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&&) = delete;
    FdGuard& operator=(FdGuard&&) = delete;
    ~FdGuard() { reset(); }
    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

// Raises `fd` to >= 3 (keeping O_CLOEXEC) so dup2() onto 0..2 can never alias it. -1 stays -1.
inline int lift_fd(int fd) noexcept {
    if (fd < 0 || fd > 2) {
        return fd;
    }
    const int lifted = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    ::close(fd);
    return lifted;
}

// Highest fd + 1 to close by hand when close_range() is unavailable (capped at 2^20).
inline unsigned int compute_fd_limit() noexcept {
    rlimit limit{};
    constexpr rlim_t kCap = 1U << 20U;
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY ||
        limit.rlim_cur > kCap) {
        return static_cast<unsigned int>(kCap);
    }
    return static_cast<unsigned int>(limit.rlim_cur);
}

inline void close_range_fallback(unsigned int first, unsigned int last_inclusive,
                                 unsigned int fd_limit) noexcept {
    for (unsigned int fd = first; fd <= last_inclusive && fd < fd_limit; ++fd) {
        ::close(static_cast<int>(fd));
    }
}

// Closes every fd >= 3 except `keep` (which must be >= 3). Async-signal-safe.
inline void close_fds_except(int keep, unsigned int fd_limit) noexcept {
    const auto keep_u = static_cast<unsigned int>(keep);
#ifdef SYS_close_range
    bool done = true;
    if (keep_u > 3 && ::syscall(SYS_close_range, 3U, keep_u - 1, 0U) != 0) {
        done = false;
    }
    if (done && ::syscall(SYS_close_range, keep_u + 1, ~0U, 0U) == 0) {
        return;
    }
#endif
    if (keep_u > 3) {
        close_range_fallback(3, keep_u - 1, fd_limit);
    }
    close_range_fallback(keep_u + 1, UINT_MAX, fd_limit);
}

} // namespace navigator::platform_internal
