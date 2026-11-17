// navigator/platform.hpp
//
// Purpose: desktop integration: open with default app, trash, MIME lookup, terminal, XDG dirs
//          (base dirs, user dirs from user-dirs.dirs), passwd home lookup.
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
#include <string_view>
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

// XDG user/base directory. Reads the environment (and, for the user dirs, the user-dirs.dirs
// file) on every call (no caching), so tests may set HOME / XDG_* before calling. The directory
// is not required to exist.
//   Home   (Phase 2): $HOME if it is absolute and non-empty, else the passwd entry of the real
//                     uid (getpwuid_r). Errors: NotFound (neither available).
//   Config (Phase 2): $XDG_CONFIG_HOME if absolute, else Home/.config.
//   Cache  (Phase 2): $XDG_CACHE_HOME if absolute, else Home/.cache.
//   Desktop, Documents, Downloads, Music, Pictures, Videos (Phase 4): the matching field of
//                     read_user_dirs(Home, Config). Errors: NotFound (the field is empty, i.e.
//                     not configured), plus those of Home and read_user_dirs. A directory the
//                     file sets to $HOME ("disabled" in xdg-user-dirs terms) returns Home.
// Blocking: one small file read for the user dirs; Home may consult NSS (getpwuid_r).
[[nodiscard]] Result<Path> user_dir(UserDir which);

// ---------------------------------------------------------------------------------------------
// XDG user directories (Phase 4: sidebar Places, GoTo common paths)
// ---------------------------------------------------------------------------------------------
//
// Source: "$XDG_CONFIG_HOME/user-dirs.dirs" (default ~/.config/user-dirs.dirs), written by
// xdg-user-dirs-update in the user's language (Turkish installs have e.g. "$HOME/Masaüstü",
// "$HOME/İndirilenler"), so the names must be read, never guessed.

// One field per XDG_<NAME>_DIR key. Empty Path: not set by the file (and no default applies).
struct UserDirs {
    Path desktop;      // XDG_DESKTOP_DIR
    Path documents;    // XDG_DOCUMENTS_DIR
    Path downloads;    // XDG_DOWNLOAD_DIR   (singular in the file format)
    Path music;        // XDG_MUSIC_DIR
    Path pictures;     // XDG_PICTURES_DIR
    Path videos;       // XDG_VIDEOS_DIR
    Path templates;    // XDG_TEMPLATES_DIR   (parsed; not shown in Places)
    Path public_share; // XDG_PUBLICSHARE_DIR (parsed; not shown in Places)

    friend bool operator==(const UserDirs&, const UserDirs&) = default;
};

// Parses the text of a user-dirs.dirs file (user-dirs.dirs(5), as GLib's
// g_get_user_special_dir reads it). Pure; no I/O, no environment access.
//   - Lines are separated by '\n'. Leading spaces/tabs are skipped; empty lines and lines whose
//     first non-blank character is '#' are ignored.
//   - A setting is   XDG_<NAME>_DIR="<value>"   with no spaces around '='; the value is a
//     double-quoted string in which '\' escapes the next character (so \" \\ \$ \` give the
//     character itself). Anything after the closing quote is ignored.
//   - <value> must be "$HOME" or start with "$HOME/" (replaced by `home`) or start with '/'
//     (absolute). Any other value (relative, other variables, missing quotes) ignores the line.
//   - The result is lexically normalized (std::filesystem::path::lexically_normal) with a
//     trailing separator removed, so "$HOME/" gives exactly `home`.
//   - Unknown keys are ignored; a key given twice: the last valid line wins.
//   - Default (GLib behaviour): when XDG_DESKTOP_DIR is not set, desktop = home / "Desktop".
//     No other field has a default.
// A field equal to `home` means "disabled" (xdg-user-dirs sets a missing folder to $HOME); it is
// kept as is here, and places::build_places() leaves it out.
// Errors: InvalidArgument (`home` empty or relative), OutOfMemory.
[[nodiscard]] Result<UserDirs> parse_user_dirs(std::string_view text, const Path& home);

// Reads "<config_home>/user-dirs.dirs" and parses it with parse_user_dirs(text, home). The
// roots are explicit so tests use a temporary directory; production code passes
// user_dir(Home) and user_dir(Config).
// A missing file is not an error: the result is parse_user_dirs("", home) (desktop default
// only). Files larger than 64 KiB are treated as empty (defaults). Blocking (one small read).
// Errors: InvalidArgument (empty or relative home/config_home), PermissionDenied / IoError
// (the file exists but cannot be read), OutOfMemory.
[[nodiscard]] Result<UserDirs> read_user_dirs(const Path& home, const Path& config_home);

// Home directory of the user named `user_name` (passwd database, getpwnam_r), for "~name" in
// the GoTo box. May block on NSS (LDAP, SSSD): call it off the UI thread (the GoTo resolution
// job runs on a worker, docs/architecture.md "Phase 4").
// Errors: InvalidArgument (empty name or one containing '/' or NUL), NotFound (no such user, or
// the entry has an empty or relative home), PlatformError (NSS failure), OutOfMemory.
[[nodiscard]] Result<Path> user_home(std::string_view user_name);

} // namespace navigator::platform
