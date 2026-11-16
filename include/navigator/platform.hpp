// navigator/platform.hpp
//
// Purpose: desktop integration: open with default app, trash, MIME lookup, terminal, XDG dirs.
//          Backends (GIO, portals, xdg-open, ...) stay inside src/platform; none leak here.
//          Pure MIME guessing and Type-column labels live in navigator/mime.hpp.
// Owner:   contracts (header); implementation in src/platform (role: platform).
// Thread-safety: free functions are reentrant (no shared mutable state) and may be called from
//                any thread. Unless a function says otherwise it may block (D-Bus, process spawn,
//                file I/O): call it off the UI thread or through the ops/job system.
//                IAppLauncher: see the class comment.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <navigator/types.hpp>

namespace navigator::platform {

// ---------------------------------------------------------------------------------------------
// Launching programs (Phase 2)
// ---------------------------------------------------------------------------------------------

// A program to start. argv[0] is a program name looked up in $PATH (execvp rules: a name with a
// '/' is used as is). Arguments are passed verbatim as bytes (no shell, no quoting, no URI
// conversion). working_dir empty: the child inherits Navigator's current directory.
struct LaunchRequest {
    std::vector<std::string> argv;
    Path working_dir;
};

// Starts programs detached from Navigator. Production code uses create_default_launcher(); tests
// inject a fake that records requests.
//
// Contract of launch() for every implementation:
//   - Returns once the program has been started (or failed to start); it never waits for the
//     program to exit. Success means "exec succeeded", not "the program did what was asked".
//   - The started program is detached: it runs in its own session (setsid), is not left as a
//     child or zombie of Navigator, and survives Navigator exiting.
//   - It inherits no file descriptors except 0, 1, 2, which are redirected to /dev/null.
//   - It inherits Navigator's environment (DISPLAY, WAYLAND_DISPLAY, XDG_* are needed by
//     openers) and default signal dispositions/mask.
// Errors: InvalidArgument (empty argv or empty argv[0]), NotFound (argv[0] not found in $PATH
// or not executable), PlatformError (fork/spawn/exec failure; message carries strerror text).
//
// Thread-safety: the default launcher holds no mutable state; its launch() may be called
// concurrently from several threads. Fakes may be single-threaded; document per fake.
class IAppLauncher {
public:
    IAppLauncher() = default;
    virtual ~IAppLauncher() = default;
    IAppLauncher(const IAppLauncher&) = delete;
    IAppLauncher& operator=(const IAppLauncher&) = delete;
    IAppLauncher(IAppLauncher&&) = delete;
    IAppLauncher& operator=(IAppLauncher&&) = delete;

    [[nodiscard]] virtual Result<void> launch(const LaunchRequest& request) = 0;
};

// The real launcher (fork/posix_spawn based; mechanism is an implementation detail of
// src/platform). Never fails in practice; Result for uniformity (OutOfMemory).
[[nodiscard]] Result<std::unique_ptr<IAppLauncher>> create_default_launcher();

// Opens `path` with the user's default application for its type.
//
// Steps:
//   1. `path` must be absolute (so it can never be parsed as an option) and must exist
//      (stat, following symlinks).
//   2. launcher.launch({"xdg-open", path}). xdg-open honours the desktop's defaults (GNOME,
//      Cinnamon, ...) and is in xdg-utils on every primary target.
//   3. Only if step 2 fails with NotFound (no xdg-open in $PATH): launcher.launch({"gio", "open",
//      path}).
//   4. If that also fails with NotFound: Unsupported.
// The opener runs detached; whether an application for the type exists is decided by the opener
// asynchronously and is NOT reported (xdg-open's exit status is not observed in Phase 2).
//
// UI-thread exception: this may be called from a frame-end command handler. It never waits for
// the opener; the cost is one stat() of a path the UI just listed plus one process spawn.
//
// Errors: InvalidArgument (empty or relative path), NotFound (path does not exist),
// PermissionDenied (stat EACCES), Unsupported (neither xdg-open nor gio found),
// PlatformError (spawn failure from the launcher).
[[nodiscard]] Result<void> open_with_default(const Path& path, IAppLauncher& launcher);

// Same, with a process-wide default launcher created on first use (thread-safe initialization).
[[nodiscard]] Result<void> open_with_default(const Path& path);

// ---------------------------------------------------------------------------------------------
// MIME (Phase 2: name/stat heuristic; content sniffing / GIO in a later phase)
// ---------------------------------------------------------------------------------------------

// MIME type of `path`, e.g. "image/png", "inode/directory". Phase 2: one lstat (plus one stat for
// a symlink) and then exactly the classification of mime::guess_for_entry() (navigator/mime.hpp)
// on that metadata; no file content is read. A later phase may sniff content for names the
// table does not know; the result for names it does know will not change.
// Errors: InvalidArgument (empty path), NotFound, PermissionDenied, IoError.
[[nodiscard]] Result<std::string> mime_type(const Path& path);

// ---------------------------------------------------------------------------------------------
// Stubs for later phases (declared only; do not call until their phase lands)
// ---------------------------------------------------------------------------------------------

// Ops phase. Moves `path` to the freedesktop.org trash of its volume.
// Errors: NotFound, PermissionDenied, Unsupported (no trash on this volume), IoError.
[[nodiscard]] Result<void> move_to_trash(const Path& path);

// Later phase. Opens the user's terminal emulator with `dir` as working directory.
[[nodiscard]] Result<void> open_terminal(const Path& dir);

// ---------------------------------------------------------------------------------------------
// XDG directories
// ---------------------------------------------------------------------------------------------

enum class UserDir { Home, Desktop, Documents, Downloads, Music, Pictures, Videos, Config, Cache };

// XDG user/base directory. Reads the environment on every call (no caching), so tests may set
// HOME / XDG_* before calling. The directory is not required to exist.
//   Home   (Phase 2): $HOME if it is absolute and non-empty, else the passwd entry of the real
//                     uid (getpwuid_r). Errors: NotFound (neither available).
//   Config (Phase 2): $XDG_CONFIG_HOME if absolute, else Home/.config.
//   Cache  (Phase 2): $XDG_CACHE_HOME if absolute, else Home/.cache.
//   Desktop, Documents, Downloads, Music, Pictures, Videos (Phase 4, sidebar): from
//                     Config/user-dirs.dirs; until Phase 4 they may fail with Unsupported.
[[nodiscard]] Result<Path> user_dir(UserDir which);

} // namespace navigator::platform
