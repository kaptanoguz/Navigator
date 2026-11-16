// navigator/watch.hpp
//
// Purpose: directory change notifications (inotify on Linux; polling fallback).
// Owner:   contracts (header); implementation in src/platform (role: platform). STUB until
//          Phase 1.
// Thread-safety: an IWatcher is owned by one thread. Events are delivered by polling
//                (poll_events) on the owning thread; the backend may use its own thread internally.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

enum class WatchEventKind : std::uint8_t {
    Created,
    Deleted,
    Modified,
    Renamed,   // path = new name, old_path = previous name
    Overflow,  // events were dropped; caller must re-list the directory
    WatchGone, // the watched directory was removed or unmounted
};

struct WatchEvent {
    WatchEventKind kind = WatchEventKind::Modified;
    Path path;
    Path old_path; // Renamed only
};

using WatchHandle = std::uint64_t; // 0 = invalid

class IWatcher {
public:
    virtual ~IWatcher() = default;

    // Phase 1. Non-recursive watch of one directory.
    virtual Result<WatchHandle> add(const Path& dir) = 0;
    virtual void remove(WatchHandle handle) = 0;
    // Phase 1. Non-blocking; returns events collected since the last call.
    virtual std::vector<WatchEvent> poll_events() = 0;
};

// Phase 1. Errors: PlatformError (inotify unavailable or limit reached), Unsupported.
[[nodiscard]] Result<std::unique_ptr<IWatcher>> create_watcher();

} // namespace navigator
