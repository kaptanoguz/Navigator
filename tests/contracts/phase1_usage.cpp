// Compile-only check that the Phase 1 contracts (fs, watch, volume) are usable as documented and
// that IWatcher / IMountSource / IVolumeMonitor can be implemented by test fakes. Nothing here is
// linked or run.
#include <chrono>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>
#include <navigator/watch.hpp>

namespace navigator::contracts_check {

static_assert(fs::is_hidden_name(".bashrc"));
static_assert(!fs::is_hidden_name("a.txt"));
static_assert(fs::extension("a.tar.gz") == "gz");
static_assert(fs::extension(".bashrc").empty());
static_assert(fs::extension("noext").empty());
static_assert(fs::extension("x.").empty());

class FakeWatcher final : public IWatcher {
public:
    Result<WatchHandle> add(const Path& dir) override {
        if (dir.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty");
        }
        return ++next_;
    }
    void remove(WatchHandle /*handle*/) noexcept override {}
    Result<std::size_t> poll(std::vector<WatchEvent>& out) override {
        const std::size_t n = pending_.size();
        for (WatchEvent& event : pending_) {
            out.push_back(std::move(event));
        }
        pending_.clear();
        return n;
    }
    bool wait(std::chrono::milliseconds /*timeout*/) override { return !pending_.empty(); }
    int native_handle() const noexcept override { return -1; }

    void push(WatchEvent event) { pending_.push_back(std::move(event)); }

private:
    WatchHandle next_ = kInvalidWatchHandle;
    std::vector<WatchEvent> pending_;
};

class FakeMountSource final : public IMountSource {
public:
    explicit FakeMountSource(std::string text) : text_(std::move(text)) {}
    Result<std::string> read_mountinfo() override { return text_; }
    Result<SpaceInfo> query_space(const Path& /*mount_point*/) override {
        return SpaceInfo{100, 50, 40};
    }

private:
    std::string text_;
};

std::size_t use_fs(std::stop_token stop) {
    fs::ListOptions options;
    options.show_hidden = true;
    Result<std::vector<fs::DirEntry>> listed = fs::list_directory("/tmp", options, std::move(stop));
    if (!listed) {
        return listed.error().code == ErrorCode::Cancelled ? 0U : 1U;
    }
    fs::SortOptions sort;
    sort.key = fs::SortKey::Modified;
    sort.descending = true;
    fs::sort_entries(*listed, sort);
    std::size_t dirs = 0;
    for (const fs::DirEntry& entry : *listed) {
        if (fs::is_directory_like(entry) && !entry.metadata_error && entry.readable) {
            ++dirs;
        }
        if (entry.kind == fs::EntryKind::Symlink && entry.dangling) {
            ++dirs;
        }
        dirs += fs::to_display_name(entry.name).empty() ? 0U : 1U;
        dirs += static_cast<std::size_t>(entry.mtime.time_since_epoch().count() > 0);
    }
    const Result<fs::DirEntry> one = fs::stat("/", true);
    const bool lt = fs::compare_names("file9", "file10", fs::SortOptions{}) < 0;
    return dirs + (one ? 1U : 0U) + (lt ? 1U : 0U) + fs::fold_name("İ").size() +
           fs::list_directory("/").value_or(std::vector<fs::DirEntry>{}).size();
}

std::size_t use_watch() {
    WatcherFactory factory = []() -> Result<std::unique_ptr<IWatcher>> {
        return std::unique_ptr<IWatcher>(std::make_unique<FakeWatcher>());
    };
    Result<std::unique_ptr<IWatcher>> watcher = factory();
    if (!watcher) {
        watcher = create_inotify_watcher();
    }
    if (!watcher) {
        return 0;
    }
    IWatcher& w = **watcher;
    const Result<WatchHandle> handle = w.add("/tmp");
    std::vector<WatchEvent> events;
    std::size_t total = 0;
    if (w.wait(std::chrono::milliseconds{0})) {
        total += w.poll(events).value_or(0);
    }
    for (const WatchEvent& event : events) {
        if (event.kind == WatchEventKind::Renamed) {
            total += event.old_name.size() + event.name.size();
        }
    }
    if (handle) {
        w.remove(*handle);
    }
    return total + static_cast<std::size_t>(w.native_handle() >= 0);
}

std::size_t use_volume() {
    const std::vector<MountEntry> entries = parse_mountinfo(
        "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw,errors=remount-ro\n");
    VolumeQuery query;
    query.source = std::make_shared<FakeMountSource>("");
    query.sysfs_root = "/nonexistent/sys";
    query.query_space = false;
    std::size_t total = entries.size();
    if (Result<std::vector<VolumeInfo>> volumes = list_volumes(query)) {
        for (const VolumeInfo& v : *volumes) {
            total += v.kind == VolumeKind::Removable ? 1U : 0U;
            total += v.uuid.value_or("").size();
            total += v.has_space ? static_cast<std::size_t>(v.space.available_bytes) : 0U;
        }
    }
    total += volume_for("/home", query) ? 1U : 0U;
    total += is_pseudo_filesystem("proc") ? 1U : 0U;
    total += is_network_filesystem("nfs4") ? 1U : 0U;
    if (Result<std::unique_ptr<IVolumeMonitor>> monitor = create_volume_monitor(query)) {
        std::vector<VolumeEvent> events;
        total += (*monitor)->poll(events).value_or(0) + (*monitor)->volumes().size();
    }
    total += create_proc_mount_source() ? 1U : 0U;
    return total;
}

} // namespace navigator::contracts_check
