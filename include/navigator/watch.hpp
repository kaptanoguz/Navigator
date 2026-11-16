// navigator/watch.hpp
//
// Purpose: directory change notifications. Phase 1 backend: inotify. Consumers depend only on
//          IWatcher so tests and later backends (polling, fanotify, GVfs) can be injected.
// Owner:   contracts (header); implementation in src/platform (role: platform). Phase 1.
// Thread-safety: an IWatcher is NOT thread-safe. All member functions of one watcher must be
//                called from a single thread at a time (normally the thread that created it);
//                handing a watcher to another thread is fine with ordinary happens-before
//                synchronization. Different watchers are independent. The inotify backend uses
//                no internal thread: events wait in the kernel queue and in a small internal
//                pairing buffer until poll() drains them on the consumer's thread. A consumer
//                that wants push delivery runs a worker that blocks in wait() and forwards
//                poll() results (for the UI: as a frame-start message, never mid-draw).
// Errors:  navigator::Result (types.hpp); no exceptions escape.
//
// Event delivery model
//   - Non-recursive: one watch observes the direct children of one directory and the directory
//     itself (Attrib with empty name, WatchedDirGone).
//   - Events are appended in kernel order. Names are raw bytes, as in fs::DirEntry::name.
//   - Rename pairing: a MovedFrom and a MovedTo with the same inotify cookie on the SAME handle
//     become one Renamed{old_name, name}. Moves between two watched directories, or into/out of
//     unwatched ones, are reported as MovedFrom on the source handle and/or MovedTo on the
//     destination handle; treat MovedFrom like Deleted and MovedTo like Created. An unpaired
//     MovedFrom is held at most until the end of the poll() that read it (it is never delayed to
//     a later poll()).
//   - Modified may arrive many times per write; a backend may coalesce identical consecutive
//     Modified events for the same handle and name within one poll().
//   - Overflow (kernel IN_Q_OVERFLOW or internal queue limit): events were lost for all handles;
//     the consumer must re-list every watched directory. Watches stay active.
//   - WatchedDirGone: the watched directory was deleted, moved/renamed (its path is now stale),
//     its filesystem was unmounted, or the kernel dropped the watch. Emitted at most once per
//     handle; the handle is then invalid and already removed (remove() on it is a no-op).
//   - Coverage limits (document to users, do not paper over): network and FUSE filesystems
//     (NFS, SMB/CIFS, sshfs, GVfs FUSE) report only local changes made through this kernel;
//     /proc, /sys and similar report nothing; changes inside subdirectories are not reported.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

// Opaque watch identifier, unique for the lifetime of one IWatcher (never reused, unlike inotify
// watch descriptors). 0 is invalid.
using WatchHandle = std::uint64_t;
inline constexpr WatchHandle kInvalidWatchHandle = 0;

enum class WatchEventKind : std::uint8_t {
    Created = 0,        // IN_CREATE: name appeared (file, dir, link, ...)
    Deleted = 1,        // IN_DELETE
    Modified = 2,       // IN_MODIFY / IN_CLOSE_WRITE: content changed
    Attrib = 3,         // IN_ATTRIB: metadata (mode, owner, times, link count, xattrs); empty name
                        //            means the watched directory itself
    MovedFrom = 4,      // IN_MOVED_FROM without a matching MovedTo on the same handle
    MovedTo = 5,        // IN_MOVED_TO without a matching MovedFrom on the same handle
    Renamed = 6,        // paired MovedFrom+MovedTo on the same handle: old_name -> name
    Overflow = 7,       // events lost; handle == kInvalidWatchHandle; re-list everything
    WatchedDirGone = 8, // IN_DELETE_SELF / IN_MOVE_SELF / IN_UNMOUNT / IN_IGNORED; name empty
};

struct WatchEvent {
    WatchEventKind kind = WatchEventKind::Modified;
    WatchHandle handle = kInvalidWatchHandle; // watch the event belongs to (invalid for Overflow)
    std::string name;     // child name (raw bytes); for Renamed the new name; else may be empty
    std::string old_name; // Renamed only: previous name
    bool is_directory = false; // the child is a directory (IN_ISDIR); false when unknown
};

class IWatcher {
public:
    IWatcher() = default;
    IWatcher(const IWatcher&) = delete;
    IWatcher& operator=(const IWatcher&) = delete;
    IWatcher(IWatcher&&) = delete;
    IWatcher& operator=(IWatcher&&) = delete;
    virtual ~IWatcher() = default;

    // Starts watching the direct children of `dir` (followed if it is a symlink). Adding a
    // directory that is already watched (same inode) returns the same handle and increments a
    // reference count; each add() needs a matching remove().
    // Errors: InvalidArgument (empty path, not a directory), NotFound, PermissionDenied,
    //         PlatformError (watch limit: message names fs.inotify.max_user_watches).
    [[nodiscard]] virtual Result<WatchHandle> add(const Path& dir) = 0;

    // Drops one reference. When the last reference goes, the watch is removed and events for the
    // handle that are queued but not yet polled are discarded. Unknown, invalid or already-gone
    // handles are ignored.
    virtual void remove(WatchHandle handle) noexcept = 0;

    // Non-blocking. Appends all currently available events to `out` (existing contents are kept)
    // and returns how many were appended (0 when none).
    // Errors: IoError if reading the backend fails; the watcher stays usable and a subsequent
    //         successful poll() starts with an Overflow event.
    [[nodiscard]] virtual Result<std::size_t> poll(std::vector<WatchEvent>& out) = 0;

    // Blocks until poll() would return at least one event (or an error), or until `timeout`
    // elapses. timeout == 0 only checks; timeout < 0 waits without limit. Returns true if events
    // are ready. EINTR is retried internally (with the remaining time).
    [[nodiscard]] virtual bool wait(std::chrono::milliseconds timeout) = 0;

    // A file descriptor that becomes readable (POLLIN) whenever wait() would return true, for
    // integration into an external event loop. Owned by the watcher; do not read or close it.
    // -1 when the backend has none (fakes): use wait()/poll() instead.
    [[nodiscard]] virtual int native_handle() const noexcept = 0;
};

// Creates the inotify backend (inotify_init1(IN_NONBLOCK | IN_CLOEXEC)).
// Errors: PlatformError (EMFILE: fs.inotify.max_user_instances reached; other errno),
//         Unsupported (kernel without inotify).
[[nodiscard]] Result<std::unique_ptr<IWatcher>> create_inotify_watcher();

// Injection point: code that needs watchers takes a factory rather than calling
// create_inotify_watcher() directly, so tests can supply a fake IWatcher.
using WatcherFactory = std::function<Result<std::unique_ptr<IWatcher>>()>;

} // namespace navigator
