// Internal (src/platform): the inotify IWatcher backend. Exposed only so tests/platform can reach
// the test seams below (synthetic kernel records, forced read errors); production code uses
// navigator::create_inotify_watcher() from <navigator/watch.hpp>.
//
// Thread-safety: as IWatcher (one thread at a time). The test seams follow the same rule.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <navigator/watch.hpp>

namespace navigator::platform_internal {

// One decoded `struct inotify_event`.
struct RawInotifyEvent {
    int wd = -1;
    std::uint32_t mask = 0;
    std::uint32_t cookie = 0;
    std::string name;
};

class InotifyWatcher final : public IWatcher {
public:
    // inotify_init1(IN_NONBLOCK | IN_CLOEXEC).
    [[nodiscard]] static Result<std::unique_ptr<InotifyWatcher>> create();

    InotifyWatcher(const InotifyWatcher&) = delete;
    InotifyWatcher& operator=(const InotifyWatcher&) = delete;
    InotifyWatcher(InotifyWatcher&&) = delete;
    InotifyWatcher& operator=(InotifyWatcher&&) = delete;
    ~InotifyWatcher() override;

    [[nodiscard]] Result<WatchHandle> add(const Path& dir) override;
    void remove(WatchHandle handle) noexcept override;
    [[nodiscard]] Result<std::size_t> poll(std::vector<WatchEvent>& out) override;
    [[nodiscard]] bool wait(std::chrono::milliseconds timeout) override;
    [[nodiscard]] int native_handle() const noexcept override { return fd_; }

    // ---- Test seams (not part of the public contract) ----
    // Queues a synthetic kernel record; the next poll() processes it before the kernel's own.
    void test_inject(RawInotifyEvent event);
    // The next poll() fails as if read(2) returned `err`.
    void test_fail_next_read(int err) noexcept { fail_next_read_ = err; }
    // Kernel watch descriptor behind a live handle, or -1.
    [[nodiscard]] int test_wd(WatchHandle handle) const noexcept;
    // Number of live handles.
    [[nodiscard]] std::size_t test_live_handles() const noexcept { return by_handle_.size(); }

    // poll() stops reading the kernel queue after this many bytes so a busy writer cannot keep
    // one poll() running forever; the rest stays queued and wait() reports it immediately.
    static constexpr std::size_t kMaxBytesPerPoll = std::size_t{4} << 20U;

private:
    explicit InotifyWatcher(int fd) noexcept : fd_(fd) {}

    struct Watch {
        WatchHandle handle = kInvalidWatchHandle;
        std::size_t refs = 0;
    };

    Result<void> read_kernel(std::vector<RawInotifyEvent>& raw);
    void translate(const std::vector<RawInotifyEvent>& raw, std::vector<WatchEvent>& out,
                   std::size_t base);
    // Drops the watch for `wd` (kernel + maps). Exactly one IN_IGNORED for `wd` is then still
    // to come from the kernel; every record for `wd` up to and including it is discarded.
    void drop_watch(int wd) noexcept;

    int fd_ = -1;
    WatchHandle next_handle_ = 1;
    std::unordered_map<int, Watch> by_wd_;
    std::unordered_map<WatchHandle, int> by_handle_;
    std::unordered_map<int, std::uint32_t> stale_wds_; // wd -> IN_IGNORED records still expected
    std::vector<RawInotifyEvent> injected_;
    std::vector<char> buffer_;
    int fail_next_read_ = 0;
    bool overflow_pending_ = false;
};

} // namespace navigator::platform_internal
