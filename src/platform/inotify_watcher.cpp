#include "inotify_watcher.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <new>
#include <utility>

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "errno_util.hpp"

namespace navigator {

namespace platform_internal {
namespace {

constexpr std::uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE |
                                     IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                                     IN_MOVE_SELF | IN_ONLYDIR;

constexpr std::uint32_t kGoneMask = IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED;

constexpr std::size_t kReadChunk = std::size_t{64} * 1024U;

WatchEvent make_event(WatchEventKind kind, WatchHandle handle, std::string name = {},
                      bool is_directory = false) {
    WatchEvent event;
    event.kind = kind;
    event.handle = handle;
    event.name = std::move(name);
    event.is_directory = is_directory;
    return event;
}

void push_overflow(std::vector<WatchEvent>& out, std::size_t base) {
    if (out.size() > base && out.back().kind == WatchEventKind::Overflow) {
        return; // collapse consecutive overflows
    }
    out.push_back(make_event(WatchEventKind::Overflow, kInvalidWatchHandle));
}

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
        return 0;
    }
    return static_cast<int>(std::min<std::chrono::milliseconds::rep>(left.count(), INT_MAX));
}

} // namespace

Result<std::unique_ptr<InotifyWatcher>> InotifyWatcher::create() {
    const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        if (err == ENOSYS) {
            return fail(ErrorCode::Unsupported, "inotify is not supported by this kernel");
        }
        if (err == EMFILE) {
            return fail(ErrorCode::PlatformError,
                        "inotify instance limit reached (fs.inotify.max_user_instances): " +
                            errno_text(err));
        }
        return fail(ErrorCode::PlatformError, "inotify_init1 failed: " + errno_text(err));
    }
    try {
        return std::unique_ptr<InotifyWatcher>(new InotifyWatcher(fd));
    } catch (const std::bad_alloc&) {
        ::close(fd);
        return fail(ErrorCode::OutOfMemory, "inotify watcher allocation failed");
    }
}

InotifyWatcher::~InotifyWatcher() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Result<WatchHandle> InotifyWatcher::add(const Path& dir) {
    if (dir.empty()) {
        return fail(ErrorCode::InvalidArgument, "watch: empty path");
    }
    const int wd = ::inotify_add_watch(fd_, dir.c_str(), kWatchMask);
    if (wd < 0) {
        const int err = errno;
        if (err == ENOSPC) {
            return fail(ErrorCode::PlatformError,
                        "inotify watch limit reached (fs.inotify.max_user_watches) adding '" +
                            dir.native() + "'");
        }
        if (err == ENOTDIR) {
            return fail(ErrorCode::InvalidArgument,
                        "watch: not a directory '" + dir.native() + "'");
        }
        return Unexpected<Error>(
            errno_error(err, "watch: cannot add", dir, ErrorCode::PlatformError));
    }
    try {
        auto it = by_wd_.find(wd);
        if (it != by_wd_.end()) {
            ++it->second.refs; // same inode: the kernel returned the existing descriptor
            return it->second.handle;
        }
        const WatchHandle handle = next_handle_;
        by_wd_.emplace(wd, Watch{handle, 1});
        try {
            by_handle_.emplace(handle, wd);
        } catch (...) {
            by_wd_.erase(wd);
            throw;
        }
        ++next_handle_;
        return handle;
    } catch (const std::bad_alloc&) {
        ::inotify_rm_watch(fd_, wd);
        return fail(ErrorCode::OutOfMemory, "watch: allocation failed");
    }
}

void InotifyWatcher::drop_watch(int wd) noexcept {
    auto it = by_wd_.find(wd);
    if (it != by_wd_.end()) {
        by_handle_.erase(it->second.handle);
        by_wd_.erase(it);
    }
    // Whether this call or the kernel destroys the mark, exactly one IN_IGNORED follows. If the
    // mark is already gone (EINVAL), its IN_IGNORED is still unread, or we would not be here.
    ::inotify_rm_watch(fd_, wd);
    try {
        ++stale_wds_[wd];
    } catch (...) {
        // Out of memory: stale records of wd would look like records of an unknown wd and be
        // dropped anyway; only a kernel reuse of the number could misattribute them, so tell
        // the consumer to re-list.
        overflow_pending_ = true;
    }
}

void InotifyWatcher::remove(WatchHandle handle) noexcept {
    auto it = by_handle_.find(handle);
    if (it == by_handle_.end()) {
        return;
    }
    const int wd = it->second;
    auto watch = by_wd_.find(wd);
    if (watch != by_wd_.end() && watch->second.refs > 1) {
        --watch->second.refs;
        return;
    }
    drop_watch(wd);
}

void InotifyWatcher::test_inject(RawInotifyEvent event) { injected_.push_back(std::move(event)); }

int InotifyWatcher::test_wd(WatchHandle handle) const noexcept {
    auto it = by_handle_.find(handle);
    return it == by_handle_.end() ? -1 : it->second;
}

Result<void> InotifyWatcher::read_kernel(std::vector<RawInotifyEvent>& raw) {
    if (fail_next_read_ != 0) {
        const int err = std::exchange(fail_next_read_, 0);
        return fail(ErrorCode::IoError, "inotify read failed: " + errno_text(err));
    }
    if (buffer_.size() < kReadChunk) {
        buffer_.resize(kReadChunk);
    }
    std::size_t total = 0;
    while (total < kMaxBytesPerPoll) {
        const ssize_t n = ::read(fd_, buffer_.data(), buffer_.size());
        if (n < 0) {
            const int err = errno;
            if (err == EINTR) {
                continue;
            }
            if (err == EAGAIN || err == EWOULDBLOCK) {
                break;
            }
            return fail(ErrorCode::IoError, "inotify read failed: " + errno_text(err));
        }
        if (n == 0) {
            break;
        }
        const auto len = static_cast<std::size_t>(n);
        total += len;
        std::size_t offset = 0;
        while (offset + sizeof(inotify_event) <= len) {
            inotify_event header{};
            std::memcpy(&header, buffer_.data() + offset, sizeof header);
            const std::size_t name_off = offset + sizeof(inotify_event);
            if (name_off + header.len > len) {
                break; // truncated record: cannot happen with a kernel-sized buffer
            }
            RawInotifyEvent event;
            event.wd = header.wd;
            event.mask = header.mask;
            event.cookie = header.cookie;
            if (header.len > 0) {
                const char* name = buffer_.data() + name_off;
                event.name.assign(name, ::strnlen(name, header.len)); // strip NUL padding
            }
            raw.push_back(std::move(event));
            offset = name_off + header.len;
        }
    }
    return ok();
}

void InotifyWatcher::translate(const std::vector<RawInotifyEvent>& raw,
                               std::vector<WatchEvent>& out, std::size_t base) {
    // cookie -> index in `out` of a MovedFrom read in this poll() and not yet paired.
    std::unordered_map<std::uint32_t, std::size_t> pending_from;

    for (const RawInotifyEvent& ev : raw) {
        if ((ev.mask & IN_Q_OVERFLOW) != 0U) {
            push_overflow(out, base);
            continue;
        }
        if (auto stale = stale_wds_.find(ev.wd); stale != stale_wds_.end()) {
            // Records of a removed watch, queued before its IN_IGNORED: discarded.
            if ((ev.mask & IN_IGNORED) != 0U && --stale->second == 0) {
                stale_wds_.erase(stale);
            }
            continue;
        }
        auto it = by_wd_.find(ev.wd);
        if (it == by_wd_.end()) {
            continue;
        }
        const WatchHandle handle = it->second.handle;
        const bool is_dir = (ev.mask & IN_ISDIR) != 0U;

        if ((ev.mask & kGoneMask) != 0U) {
            out.push_back(make_event(WatchEventKind::WatchedDirGone, handle));
            if ((ev.mask & IN_IGNORED) != 0U) {
                // The kernel already destroyed the mark; nothing more will come for wd.
                by_handle_.erase(handle);
                by_wd_.erase(it);
            } else {
                // DELETE_SELF / UNMOUNT are followed by IN_IGNORED; MOVE_SELF is not, so the
                // watch is removed here (the path is stale) and its IN_IGNORED swallowed.
                drop_watch(ev.wd);
            }
            continue;
        }
        if ((ev.mask & IN_MOVED_FROM) != 0U) {
            const std::size_t index = out.size();
            out.push_back(make_event(WatchEventKind::MovedFrom, handle, ev.name, is_dir));
            if (ev.cookie != 0U) {
                pending_from[ev.cookie] = index;
            }
            continue;
        }
        if ((ev.mask & IN_MOVED_TO) != 0U) {
            if (ev.cookie != 0U) {
                auto from = pending_from.find(ev.cookie);
                if (from != pending_from.end()) {
                    WatchEvent& prev = out[from->second];
                    pending_from.erase(from);
                    if (prev.handle == handle && prev.kind == WatchEventKind::MovedFrom) {
                        prev.kind = WatchEventKind::Renamed;
                        prev.old_name = std::move(prev.name);
                        prev.name = ev.name;
                        prev.is_directory = prev.is_directory || is_dir;
                        continue;
                    }
                }
            }
            out.push_back(make_event(WatchEventKind::MovedTo, handle, ev.name, is_dir));
            continue;
        }
        if ((ev.mask & IN_CREATE) != 0U) {
            out.push_back(make_event(WatchEventKind::Created, handle, ev.name, is_dir));
        } else if ((ev.mask & IN_DELETE) != 0U) {
            out.push_back(make_event(WatchEventKind::Deleted, handle, ev.name, is_dir));
        } else if ((ev.mask & (IN_MODIFY | IN_CLOSE_WRITE)) != 0U) {
            if (out.size() > base) {
                const WatchEvent& last = out.back();
                if (last.kind == WatchEventKind::Modified && last.handle == handle &&
                    last.name == ev.name) {
                    continue; // coalesce identical consecutive Modified
                }
            }
            out.push_back(make_event(WatchEventKind::Modified, handle, ev.name, is_dir));
        } else if ((ev.mask & IN_ATTRIB) != 0U) {
            out.push_back(make_event(WatchEventKind::Attrib, handle, ev.name, is_dir));
        }
    }
}

Result<std::size_t> InotifyWatcher::poll(std::vector<WatchEvent>& out) {
    const std::size_t base = out.size();
    try {
        std::vector<RawInotifyEvent> raw = std::exchange(injected_, {});
        if (Result<void> read = read_kernel(raw); !read) {
            overflow_pending_ = true; // records already read from the kernel are lost
            return Unexpected<Error>(read.error());
        }
        if (overflow_pending_) {
            push_overflow(out, base);
            overflow_pending_ = false;
        }
        translate(raw, out, base);
        return out.size() - base;
    } catch (const std::bad_alloc&) {
        out.resize(base);
        overflow_pending_ = true;
        return fail(ErrorCode::OutOfMemory, "inotify poll: allocation failed");
    }
}

bool InotifyWatcher::wait(std::chrono::milliseconds timeout) {
    if (!injected_.empty() || overflow_pending_ || fail_next_read_ != 0) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const int ms = timeout.count() < 0 ? -1 : remaining_ms(deadline);
        pollfd pfd{fd_, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, ms);
        if (rc > 0) {
            return true;
        }
        if (rc == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

} // namespace platform_internal

Result<std::unique_ptr<IWatcher>> create_inotify_watcher() {
    Result<std::unique_ptr<platform_internal::InotifyWatcher>> created =
        platform_internal::InotifyWatcher::create();
    if (!created) {
        return Unexpected<Error>(created.error());
    }
    return std::unique_ptr<IWatcher>(std::move(*created));
}

} // namespace navigator
