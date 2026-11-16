// Default IVolumeMonitor: polls the source's change_fd() for POLLPRI | POLLERR and diffs
// list_volumes() snapshots by (mount_point, mount_id).
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>

#include <navigator/volume.hpp>

#include "volume_internal.hpp"

namespace navigator {

namespace platform_internal {

namespace {

using Key = std::pair<std::string, std::uint32_t>;

Key key_of(const VolumeInfo& v) { return {v.mount_point.native(), v.mount_id}; }

bool differs(const VolumeInfo& a, const VolumeInfo& b) {
    return a.read_only != b.read_only || a.label != b.label || a.uuid != b.uuid ||
           a.device != b.device;
}

// Unmounted (old order), then Mounted / Changed (new order).
void diff_snapshots(const std::vector<VolumeInfo>& before, const std::vector<VolumeInfo>& after,
                    std::vector<VolumeEvent>& out) {
    std::map<Key, const VolumeInfo*> old_by_key;
    for (const VolumeInfo& v : before) {
        old_by_key.emplace(key_of(v), &v);
    }
    std::map<Key, const VolumeInfo*> new_by_key;
    for (const VolumeInfo& v : after) {
        new_by_key.emplace(key_of(v), &v);
    }
    for (const VolumeInfo& v : before) {
        if (!new_by_key.contains(key_of(v))) {
            out.push_back(VolumeEvent{VolumeEventKind::Unmounted, v});
        }
    }
    for (const VolumeInfo& v : after) {
        auto it = old_by_key.find(key_of(v));
        if (it == old_by_key.end()) {
            out.push_back(VolumeEvent{VolumeEventKind::Mounted, v});
        } else if (differs(*it->second, v)) {
            out.push_back(VolumeEvent{VolumeEventKind::Changed, v});
        }
    }
}

int to_poll_ms(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return -1;
    }
    return static_cast<int>(std::min<std::chrono::milliseconds::rep>(timeout.count(), INT_MAX));
}

// Polls `fd` for a mount-table signal. Returns true when POLLPRI or POLLERR is reported.
bool poll_change_fd(int fd, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int ms = to_poll_ms(timeout);
        if (ms > 0) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            ms = to_poll_ms(std::max(left, std::chrono::milliseconds{0}));
        }
        pollfd pfd{fd, POLLPRI, 0};
        const int rc = ::poll(&pfd, 1, ms);
        if (rc > 0) {
            return (pfd.revents & (POLLPRI | POLLERR)) != 0;
        }
        if (rc == 0 || errno != EINTR) {
            return false;
        }
    }
}

class VolumeMonitor final : public IVolumeMonitor {
public:
    VolumeMonitor(VolumeQuery query, std::vector<VolumeInfo> snapshot, std::string text)
        : query_(std::move(query)), snapshot_(std::move(snapshot)), last_text_(std::move(text)) {}

    Result<std::size_t> poll(std::vector<VolumeEvent>& out) override {
        const std::size_t base = out.size();
        try {
            const int fd = query_.source->change_fd();
            bool signalled = std::exchange(signalled_, false);
            if (fd >= 0 && poll_change_fd(fd, std::chrono::milliseconds{0})) {
                signalled = true;
            }
            Result<std::string> text = query_.source->read_mountinfo();
            if (!text) {
                signalled_ = signalled_ || signalled;
                return Unexpected<Error>(text.error());
            }
            // With a change_fd, an unchanged table means nothing to diff: skip statvfs/sysfs.
            // The text comparison also catches signals consumed by an external event loop
            // polling native_handle(). Without a change_fd the pipeline runs every time.
            if (fd >= 0 && *text == last_text_) {
                return std::size_t{0};
            }
            Result<std::vector<VolumeInfo>> volumes = build_volumes(*text, query_, *query_.source);
            if (!volumes) {
                signalled_ = signalled_ || signalled;
                return Unexpected<Error>(volumes.error());
            }
            diff_snapshots(snapshot_, *volumes, out);
            snapshot_ = std::move(*volumes);
            last_text_ = std::move(*text);
            return out.size() - base;
        } catch (const std::bad_alloc&) {
            out.resize(base);
            return fail(ErrorCode::OutOfMemory, "volume monitor: allocation failed");
        }
    }

    bool wait(std::chrono::milliseconds timeout) override {
        const int fd = query_.source->change_fd();
        if (fd < 0) {
            if (timeout.count() > 0) {
                std::this_thread::sleep_for(timeout);
            }
            return true;
        }
        if (signalled_) {
            return true;
        }
        signalled_ = poll_change_fd(fd, timeout);
        return signalled_;
    }

    [[nodiscard]] int native_handle() const noexcept override { return query_.source->change_fd(); }

    [[nodiscard]] const std::vector<VolumeInfo>& volumes() const noexcept override {
        return snapshot_;
    }

private:
    VolumeQuery query_;
    std::vector<VolumeInfo> snapshot_;
    std::string last_text_;
    bool signalled_ = false;
};

} // namespace

} // namespace platform_internal

Result<std::unique_ptr<IVolumeMonitor>> create_volume_monitor(const VolumeQuery& query) {
    try {
        Result<std::shared_ptr<IMountSource>> source = platform_internal::resolve_source(query);
        if (!source) {
            return Unexpected<Error>(source.error());
        }
        VolumeQuery own = query;
        own.source = std::move(*source);
        Result<std::string> text = own.source->read_mountinfo();
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        Result<std::vector<VolumeInfo>> volumes =
            platform_internal::build_volumes(*text, own, *own.source);
        if (!volumes) {
            return Unexpected<Error>(volumes.error());
        }
        return std::unique_ptr<IVolumeMonitor>(std::make_unique<platform_internal::VolumeMonitor>(
            std::move(own), std::move(*volumes), std::move(*text)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "volume monitor: allocation failed");
    }
}

} // namespace navigator
