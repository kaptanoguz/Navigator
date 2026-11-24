// navigator/platform.hpp
//
// Purpose: desktop integration: open with default app, trash wrapper (trash.hpp), MIME lookup,
// terminal, XDG dirs
//          (base dirs, user dirs from user-dirs.dirs), passwd home lookup; Phase 6: thumbnail
//          cache root, XDG data dirs, $PATH lookup, and a bounded child-process runner for
//          external thumbnailers (IProcessRunner).
//          Backends (GIO, portals, xdg-open, ...) stay inside src/platform; none leak here.
//          Pure MIME guessing and Type-column labels live in navigator/mime.hpp.
// Owner:   contracts (header); implementation in src/platform (role: platform).
// Thread-safety: free functions are reentrant (no shared mutable state) and may be called from
//                any thread. Unless a function says otherwise it may block (D-Bus, process spawn,
//                file I/O): call it off the UI thread or through the ops/job system.
//                IAppLauncher: see the class comment.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
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
// Trash wrapper (Phase 7) and a stub for a later phase
// ---------------------------------------------------------------------------------------------

// Phase 7: convenience wrapper, create_trash({}) then ITrash::trash(path) (trash.hpp, rules
// T1..T7: same-filesystem rename only, never a permanent fallback). The ops engine and the ui
// use trash::ITrash (injectable, testable roots) instead; this exists for tools and tests.
// Errors: those of trash::create_trash and trash::ITrash::trash.
[[nodiscard]] Result<void> move_to_trash(const Path& path);

// Phase 10: Opens the user's terminal emulator with `dir` as working directory.
// Resolution order:
//   1. `xdg-terminal-exec` (if in $PATH)
//   2. `$TERMINAL` environment variable (if set and found in $PATH)
//   3. `x-terminal-emulator` (Debian/Ubuntu/Mint alternative)
//   4. Common Linux terminals in order: `gnome-terminal`, `kgx`, `konsole`, `xfce4-terminal`,
//      `mate-terminal`, `kitty`, `alacritty`, `wezterm`, `foot`, `tilix`, `xterm`.
// Errors: InvalidArgument (empty/relative dir), NotFound (dir does not exist),
//         Unsupported (no terminal emulator found), PlatformError (spawn failed).
[[nodiscard]] Result<void> open_terminal(const Path& dir, IAppLauncher& launcher);
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

// ---------------------------------------------------------------------------------------------
// Phase 6: thumbnail cache location, data dirs, program lookup, bounded child processes
// ---------------------------------------------------------------------------------------------

// Freedesktop thumbnail cache root: user_dir(Cache) / "thumbnails" (~/.cache/thumbnails,
// honours $XDG_CACHE_HOME). Shared with every other program that follows the Thumbnail
// Managing Standard. Not required to exist. Errors: those of user_dir(Cache).
// AppConfig::thumbnail_dir / cache_dir override it in the app (docs/architecture.md "Phase 6").
[[nodiscard]] Result<Path> thumbnail_cache_dir();

// XDG data directories in precedence order: $XDG_DATA_HOME if absolute (else Home/.local/share),
// then each absolute entry of $XDG_DATA_DIRS (':'-separated; unset or empty:
// "/usr/local/share:/usr/share"), relative entries skipped, duplicates (lexically normal) kept
// once at their first position. Read on every call (no caching). Not required to exist.
// Errors: those of user_dir(Home).
[[nodiscard]] Result<std::vector<Path>> data_dirs();

// $PATH lookup with execvp rules: a name containing '/' is checked as is; otherwise each
// $PATH entry (empty entries = current directory, unset PATH = confstr(_CS_PATH)) is tried for
// a regular file with an execute bit for the effective user (access X_OK). Blocking (stat).
// Errors: InvalidArgument (empty name or a NUL byte), NotFound.
[[nodiscard]] Result<Path> find_program(std::string_view name);

// A child process that Navigator waits for (external thumbnailers), unlike LaunchRequest which
// is detached. Never through a shell: argv is passed verbatim to execvp.
struct ProcessRequest {
    std::vector<std::string> argv; // argv[0] looked up as find_program() does
    Path working_dir;              // empty: inherit
    // Wall-clock limit; when it passes, the whole process group gets SIGKILL and the result says
    // timed_out. Must be > 0.
    std::chrono::milliseconds timeout{10'000};
    // stdout and stderr are each captured up to this many bytes; the rest is read and discarded
    // (the child never blocks on a full pipe) and output_truncated is set.
    std::size_t max_capture_bytes = std::size_t{64} * 1024U;
    // Best-effort resource limits set in the child before exec (setrlimit). 0: unchanged.
    std::uint64_t address_space_bytes = 0; // RLIMIT_AS
    std::chrono::seconds cpu_time{0};      // RLIMIT_CPU
    std::uint64_t max_file_bytes = 0;      // RLIMIT_FSIZE (largest file the child may write)
    // Sandbox (best effort): when true and `bwrap` (bubblewrap) is found by find_program, the
    // child runs as bwrap --unshare-all --die-with-parent --new-session --clearenv (PATH, HOME,
    // LANG, LC_ALL kept) with read-only binds of /usr, /etc, /lib*, /bin, /sbin (as present),
    // --proc /proc, --dev /dev, --tmpfs /tmp, plus read_only_paths (--ro-bind) and
    // writable_paths (--bind), like GNOME's thumbnailer sandbox. Without bwrap the child runs
    // unsandboxed with the limits above and ProcessResult::sandboxed is false. Each runner probes
    // bwrap once (a trivial program with the same namespace flags); if the probe fails (e.g. user
    // namespaces disabled), sandboxed requests run unsandboxed with sandboxed == false. After a
    // successful probe a bwrap failure is a non-zero exit like any other and is not retried.
    bool sandbox = false;
    std::vector<Path> read_only_paths;
    std::vector<Path> writable_paths;
};

struct ProcessResult {
    int exit_code = -1;  // WEXITSTATUS when the child exited normally, else -1
    int term_signal = 0; // WTERMSIG when it was killed by a signal (incl. our SIGKILL), else 0
    bool timed_out = false;
    bool sandboxed = false;
    bool output_truncated = false;
    std::string stdout_text; // raw bytes
    std::string stderr_text; // raw bytes (logged by callers, never shown untranslated)
    std::chrono::milliseconds elapsed{0};

    [[nodiscard]] bool succeeded() const noexcept {
        return !timed_out && term_signal == 0 && exit_code == 0;
    }
};

// Runs a child to completion with a timeout. Production code uses
// create_default_process_runner(); tests inject a fake (tests/preview never spawns
// thumbnailers).
//
// Contract of run() for every implementation:
//   - Blocks the calling (worker) thread until the child is reaped. Never call it on the UI
//     thread.
//   - The child: stdin is /dev/null; stdout/stderr are pipes read by run(); no other inherited
//     fds (close-on-exec / close_range); its own process group (setpgid) so a kill reaches its
//     children; default signal dispositions and mask; Navigator's environment unless
//     sandboxed; SIGKILL if Navigator dies (PR_SET_PDEATHSIG, best effort).
//   - On timeout or when `stop` fires: SIGKILL to the process group, then reap. Timeout is a
//     success with timed_out = true; stop is Cancelled.
//   - Never leaves a zombie or a running child behind when it returns.
// Errors: InvalidArgument (empty argv / argv[0], NUL in an argument, timeout <= 0), NotFound
// (argv[0] not found or not executable), Cancelled, PlatformError (pipe/fork/spawn failure;
// message carries strerror text), OutOfMemory.
//
// Thread-safety: the default runner's only mutable state is the cached bwrap probe result,
// guarded by a mutex; run() may be called concurrently from several workers. Fakes document their
// own rule.
class IProcessRunner {
public:
    IProcessRunner() = default;
    virtual ~IProcessRunner() = default;
    IProcessRunner(const IProcessRunner&) = delete;
    IProcessRunner& operator=(const IProcessRunner&) = delete;
    IProcessRunner(IProcessRunner&&) = delete;
    IProcessRunner& operator=(IProcessRunner&&) = delete;

    [[nodiscard]] virtual Result<ProcessResult> run(const ProcessRequest& request,
                                                    std::stop_token stop) = 0;
};

// Errors: OutOfMemory.
[[nodiscard]] Result<std::unique_ptr<IProcessRunner>> create_default_process_runner();

// ---------------------------------------------------------------------------------------------
// Phase 10: GVfs mounts & remote/device URIs (MTP, SMB, SFTP, etc.)
// ---------------------------------------------------------------------------------------------

struct GvfsMount {
    std::string id;     // stable mount identifier (typically root URI)
    std::string name;   // user-visible label (e.g. "Pixel 8", "nas on server")
    std::string uri;    // root URI (e.g. "mtp://...", "smb://host/share/", "sftp://...")
    std::string scheme; // lowercase URI scheme ("mtp", "smb", "sftp", ...)
    Path local_path;    // FUSE mount path under /run/user/<uid>/gvfs/... if available, else empty
    bool can_unmount = true;

    friend bool operator==(const GvfsMount&, const GvfsMount&) = default;
};

// True when Navigator was compiled with GIO/GVfs support (NAVIGATOR_HAVE_GVFS).
[[nodiscard]] bool gvfs_available() noexcept;

// True when `raw` looks like a GVfs remote/device URI (e.g. smb://, sftp://, mtp://, ftp://,
// dav://, davs://, afc://, gphoto2://). Pure.
[[nodiscard]] bool is_gvfs_uri(std::string_view raw) noexcept;

// Abstract GVfs backend so tests can inject a mock without a live D-Bus/GVfs daemon.
// Thread-safety: methods may block on D-Bus/network I/O; call off the UI thread.
class IGvfsBackend {
public:
    IGvfsBackend() = default;
    virtual ~IGvfsBackend() = default;
    IGvfsBackend(const IGvfsBackend&) = delete;
    IGvfsBackend& operator=(const IGvfsBackend&) = delete;
    IGvfsBackend(IGvfsBackend&&) = delete;
    IGvfsBackend& operator=(IGvfsBackend&&) = delete;

    // Lists currently mounted GVfs volumes/mounts (MTP, SMB, SFTP, etc.).
    [[nodiscard]] virtual Result<std::vector<GvfsMount>> list_mounts() = 0;

    // Lists directory entries at a GVfs URI (e.g. "mtp://..." or "smb://host/share/dir").
    [[nodiscard]] virtual Result<std::vector<fs::DirEntry>> list_uri(std::string_view uri,
                                                                     std::stop_token stop = {}) = 0;

    // Resolves a GVfs URI to a local FUSE path if mounted, or returns NotFound.
    [[nodiscard]] virtual Result<Path> local_path_for_uri(std::string_view uri) = 0;

    // Unmounts the GVfs mount identified by `uri`.
    [[nodiscard]] virtual Result<void> unmount(std::string_view uri) = 0;
};

// Creates the GIO-backed GVfs backend when NAVIGATOR_HAVE_GVFS is enabled, or a clean stub
// returning empty mounts / Unsupported when built without GVfs.
[[nodiscard]] Result<std::unique_ptr<IGvfsBackend>> create_gvfs_backend();

// ---------------------------------------------------------------------------------------------
// Phase 10: Single-instance IPC (Unix domain socket)
// ---------------------------------------------------------------------------------------------

enum class IpcAction : std::uint8_t {
    FocusWindow = 0,
    OpenPath = 1,   // navigate or open path in a new tab
    NewTab = 2,     // explicitly open path in a new tab
    SelectPath = 3, // open parent folder in a tab and select the target item
};

struct IpcMessage {
    IpcAction action = IpcAction::FocusWindow;
    Path path;

    friend bool operator==(const IpcMessage&, const IpcMessage&) = default;
};

// Default per-user Unix domain socket path: "$XDG_RUNTIME_DIR/navigator.sock" (falling back to
// "/tmp/navigator-<uid>.sock").
[[nodiscard]] Result<Path> default_ipc_socket_path();

// Connects to an existing Navigator instance listening on `socket_path` and sends `message`.
// Returns true if delivered to a running instance, false if no instance is listening (including
// missing socket or ECONNREFUSED on a stale socket).
[[nodiscard]] Result<bool> send_to_running_instance(const Path& socket_path,
                                                    const IpcMessage& message);

// Non-blocking Unix domain socket listener owned by the primary Navigator instance.
// Thread-safety: poll() is non-blocking and called from the UI thread each frame.
class ISingleInstanceServer {
public:
    ISingleInstanceServer() = default;
    virtual ~ISingleInstanceServer() = default;
    ISingleInstanceServer(const ISingleInstanceServer&) = delete;
    ISingleInstanceServer& operator=(const ISingleInstanceServer&) = delete;
    ISingleInstanceServer(ISingleInstanceServer&&) = delete;
    ISingleInstanceServer& operator=(ISingleInstanceServer&&) = delete;

    // Accepts any pending connections and returns decoded messages without blocking.
    [[nodiscard]] virtual std::vector<IpcMessage> poll() = 0;

    [[nodiscard]] virtual const Path& socket_path() const noexcept = 0;
};

// Binds and listens on `socket_path` (cleaning up a stale unlistening socket if present).
// Errors: AlreadyExists (another live instance is listening), PlatformError, InvalidArgument.
[[nodiscard]] Result<std::unique_ptr<ISingleInstanceServer>>
create_single_instance_server(const Path& socket_path);

} // namespace navigator::platform
