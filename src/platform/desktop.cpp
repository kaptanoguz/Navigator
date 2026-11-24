// open_with_default(), mime_type(), user_dir() (the XDG user dirs via read_user_dirs in
// user_dirs.cpp), move_to_trash() (wrapper over trash.cpp) and the later-phase stubs of
// <navigator/platform.hpp>. No GIO: openers are external programs started through IAppLauncher.
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/mime.hpp>
#include <navigator/platform.hpp>
#include <navigator/trash.hpp>

#include "errno_util.hpp"

namespace navigator::platform {

namespace {

using platform_internal::errno_error;

// stat()/lstat() errno -> the error codes documented for open_with_default / mime_type.
Error stat_error(int err, const char* what, const Path& path) {
    Error error = errno_error(err, what, path, ErrorCode::IoError);
    if (err == ENOTDIR) {
        error.code = ErrorCode::NotFound; // a path component is a file: the path does not exist
    }
    return error;
}

fs::EntryKind kind_of(mode_t mode) noexcept {
    if (S_ISREG(mode)) {
        return fs::EntryKind::File;
    }
    if (S_ISDIR(mode)) {
        return fs::EntryKind::Directory;
    }
    if (S_ISLNK(mode)) {
        return fs::EntryKind::Symlink;
    }
    return fs::EntryKind::Other;
}

std::string_view env_value(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string_view(value) : std::string_view();
}

bool is_absolute_text(std::string_view text) noexcept {
    return !text.empty() && text.front() == '/';
}

Result<Path> home_dir() {
    if (const std::string_view home = env_value("HOME"); is_absolute_text(home)) {
        return Path(std::string(home));
    }
    long size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size <= 0) {
        size = 16384;
    }
    std::vector<char> buffer(static_cast<std::size_t>(size));
    passwd entry{};
    passwd* found = nullptr;
    int rc = 0;
    while ((rc = ::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &found)) ==
               ERANGE &&
           buffer.size() < (1U << 20U)) {
        buffer.resize(buffer.size() * 2);
    }
    if (rc == 0 && found != nullptr && found->pw_dir != nullptr && found->pw_dir[0] != '\0') {
        return Path(found->pw_dir);
    }
    return fail(ErrorCode::NotFound, "user_dir: no usable $HOME and no passwd home directory");
}

Result<Path> base_dir(const char* env_name, const char* fallback) {
    if (const std::string_view value = env_value(env_name); is_absolute_text(value)) {
        return Path(std::string(value));
    }
    auto home = home_dir();
    if (!home) {
        return home;
    }
    return *home / fallback;
}

// One XDG user directory: read_user_dirs(Home, Config), NotFound when the field is empty, Home
// itself when the file sets it to $HOME ("disabled").
Result<Path> xdg_user_dir(Path UserDirs::*field, const char* key) {
    auto home = home_dir();
    if (!home) {
        return home;
    }
    auto config = base_dir("XDG_CONFIG_HOME", ".config");
    if (!config) {
        return config;
    }
    auto dirs = read_user_dirs(*home, *config);
    if (!dirs) {
        return Unexpected<Error>(dirs.error());
    }
    Path& value = (*dirs).*field;
    if (value.empty()) {
        return fail(ErrorCode::NotFound,
                    std::string("user_dir: ") + key + " is not set in user-dirs.dirs");
    }
    std::string home_text = home->lexically_normal().native(); // as parse_user_dirs normalizes
    while (home_text.size() > 1 && home_text.back() == '/') {
        home_text.pop_back();
    }
    if (value.native() == home_text) {
        return home; // disabled: the caller sees Home exactly as user_dir(Home) returns it
    }
    return std::move(value);
}

} // namespace

Result<void> open_with_default(const Path& path, IAppLauncher& launcher) {
    try {
        if (path.empty() || !path.is_absolute()) {
            return fail(ErrorCode::InvalidArgument,
                        "open_with_default: path must be absolute: '" + path.native() + "'");
        }
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) {
            return Unexpected<Error>(stat_error(errno, "open_with_default: stat", path));
        }
        Result<void> opened = launcher.launch(LaunchRequest{{"xdg-open", path.native()}, {}});
        if (opened || opened.error().code != ErrorCode::NotFound) {
            return opened;
        }
        opened = launcher.launch(LaunchRequest{{"gio", "open", path.native()}, {}});
        if (opened || opened.error().code != ErrorCode::NotFound) {
            return opened;
        }
        return fail(ErrorCode::Unsupported,
                    "open_with_default: neither xdg-open nor gio found in $PATH");
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "open_with_default: out of memory");
    }
}

Result<void> open_with_default(const Path& path) {
    // Validate before creating the launcher, so argument errors never depend on it.
    if (path.empty() || !path.is_absolute()) {
        return fail(ErrorCode::InvalidArgument,
                    "open_with_default: path must be absolute: '" + path.native() + "'");
    }
    static const Result<std::unique_ptr<IAppLauncher>> shared = create_default_launcher();
    if (!shared) {
        return Unexpected<Error>(shared.error());
    }
    return open_with_default(path, **shared);
}

Result<std::string> mime_type(const Path& path) {
    try {
        if (path.empty()) {
            return fail(ErrorCode::InvalidArgument, "mime_type: empty path");
        }
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0) {
            return Unexpected<Error>(stat_error(errno, "mime_type: lstat", path));
        }
        fs::DirEntry entry;
        entry.name = path.filename().native();
        entry.kind = kind_of(st.st_mode);
        entry.mode = st.st_mode;
        if (entry.kind == fs::EntryKind::Symlink) {
            struct stat target {};
            if (::stat(path.c_str(), &target) == 0) {
                entry.target_kind = kind_of(target.st_mode);
                entry.mode = target.st_mode; // the target's type and permission bits
            } else {
                entry.dangling = errno == ENOENT || errno == ENOTDIR;
            }
        }
        return std::string(mime::guess_for_entry(entry));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "mime_type: out of memory");
    }
}

Result<Path> user_dir(UserDir which) {
    try {
        switch (which) {
        case UserDir::Home: return home_dir();
        case UserDir::Config: return base_dir("XDG_CONFIG_HOME", ".config");
        case UserDir::Cache: return base_dir("XDG_CACHE_HOME", ".cache");
        case UserDir::Desktop: return xdg_user_dir(&UserDirs::desktop, "XDG_DESKTOP_DIR");
        case UserDir::Documents: return xdg_user_dir(&UserDirs::documents, "XDG_DOCUMENTS_DIR");
        case UserDir::Downloads: return xdg_user_dir(&UserDirs::downloads, "XDG_DOWNLOAD_DIR");
        case UserDir::Music: return xdg_user_dir(&UserDirs::music, "XDG_MUSIC_DIR");
        case UserDir::Pictures: return xdg_user_dir(&UserDirs::pictures, "XDG_PICTURES_DIR");
        case UserDir::Videos: return xdg_user_dir(&UserDirs::videos, "XDG_VIDEOS_DIR");
        }
        return fail(ErrorCode::InvalidArgument, "user_dir: unknown UserDir value");
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "user_dir: out of memory");
    }
}

Result<void> move_to_trash(const Path& path) {
    auto trash = trash::create_trash({});
    if (!trash) {
        return Unexpected<Error>(std::move(trash.error()));
    }
    auto item = (*trash)->trash(path);
    if (!item) {
        return Unexpected<Error>(std::move(item.error()));
    }
    return ok();
}

Result<void> open_terminal(const Path& dir, IAppLauncher& launcher) {
    try {
        if (dir.empty() || !dir.is_absolute()) {
            return fail(ErrorCode::InvalidArgument,
                        "open_terminal: directory must be absolute: '" + dir.native() + "'");
        }
        struct stat st {};
        if (::stat(dir.c_str(), &st) != 0) {
            return Unexpected<Error>(stat_error(errno, "open_terminal: stat", dir));
        }
        if (!S_ISDIR(st.st_mode)) {
            return fail(ErrorCode::InvalidArgument,
                        "open_terminal: path is not a directory: '" + dir.native() + "'");
        }

        std::vector<std::string> candidates;
        candidates.emplace_back("xdg-terminal-exec");
        if (const std::string_view term = env_value("TERMINAL"); !term.empty()) {
            candidates.emplace_back(term);
        }
        candidates.emplace_back("x-terminal-emulator");
        constexpr const char* kFallbacks[] = {
            "gnome-terminal", "kgx",   "konsole",   "xfce4-terminal", "mate-terminal", "kitty",
            "alacritty",      "wezterm", "foot",    "tilix",          "xterm",
        };
        for (const char* name : kFallbacks) {
            candidates.emplace_back(name);
        }

        for (const std::string& candidate : candidates) {
            Result<void> res = launcher.launch(LaunchRequest{{candidate}, dir});
            if (res || res.error().code != ErrorCode::NotFound) {
                return res;
            }
        }
        return fail(ErrorCode::Unsupported, "open_terminal: no supported terminal emulator found");
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "open_terminal: out of memory");
    }
}

Result<void> open_terminal(const Path& dir) {
    if (dir.empty() || !dir.is_absolute()) {
        return fail(ErrorCode::InvalidArgument,
                    "open_terminal: directory must be absolute: '" + dir.native() + "'");
    }
    static const Result<std::unique_ptr<IAppLauncher>> shared = create_default_launcher();
    if (!shared) {
        return Unexpected<Error>(shared.error());
    }
    return open_terminal(dir, **shared);
}

} // namespace navigator::platform
