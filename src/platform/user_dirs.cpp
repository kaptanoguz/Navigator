// Phase 4 XDG user directories of <navigator/platform.hpp>: parse_user_dirs (user-dirs.dirs(5),
// following GLib's g_get_user_special_dir parser), read_user_dirs and user_home (getpwnam_r).
#include <array>
#include <cerrno>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#include <navigator/platform.hpp>

#include "errno_util.hpp"

namespace navigator::platform {

namespace {

using platform_internal::errno_error;

constexpr std::size_t kMaxUserDirsFile = std::size_t{64} * 1024;

struct KeyField {
    std::string_view key; // the full key, e.g. "XDG_DESKTOP_DIR"
    Path UserDirs::*field;
};

constexpr std::array<KeyField, 8> kKeys{{
    {"XDG_DESKTOP_DIR", &UserDirs::desktop},
    {"XDG_DOCUMENTS_DIR", &UserDirs::documents},
    {"XDG_DOWNLOAD_DIR", &UserDirs::downloads},
    {"XDG_MUSIC_DIR", &UserDirs::music},
    {"XDG_PICTURES_DIR", &UserDirs::pictures},
    {"XDG_VIDEOS_DIR", &UserDirs::videos},
    {"XDG_TEMPLATES_DIR", &UserDirs::templates},
    {"XDG_PUBLICSHARE_DIR", &UserDirs::public_share},
}};

// lexically_normal() without a trailing separator ("/" stays "/").
Path normalized(const Path& path) {
    std::string text = path.lexically_normal().native();
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    return {std::move(text)};
}

// Text between the opening quote (already consumed) and the first unescaped closing quote,
// with backslash escapes resolved. False when the closing quote is missing.
bool unquote(std::string_view raw, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (++i == raw.size()) {
                return false;
            }
            out.push_back(raw[i]);
        } else {
            out.push_back(c);
        }
    }
    return false;
}

// Parses one line. Returns the field it sets (nullptr: the line is ignored) and fills `value`.
Path UserDirs::*parse_line(std::string_view line, const Path& home, Path& value) {
    const std::size_t start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos || line[start] == '#') {
        return nullptr;
    }
    line.remove_prefix(start);
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
        return nullptr;
    }
    const std::string_view key = line.substr(0, eq);
    Path UserDirs::*field = nullptr;
    for (const KeyField& entry : kKeys) {
        if (entry.key == key) {
            field = entry.field;
            break;
        }
    }
    if (field == nullptr) {
        return nullptr;
    }
    std::string_view raw = line.substr(eq + 1);
    if (raw.empty() || raw.front() != '"') {
        return nullptr;
    }
    raw.remove_prefix(1);

    // The $HOME prefix is matched on the raw text (as GLib does): "\$HOME" is not a variable.
    bool relative_to_home = false;
    constexpr std::string_view kHome = "$HOME";
    if (raw.starts_with(kHome) &&
        (raw.size() == kHome.size() || raw[kHome.size()] == '/' || raw[kHome.size()] == '"')) {
        relative_to_home = true;
        raw.remove_prefix(kHome.size());
    } else if (raw.empty() || raw.front() != '/') {
        return nullptr;
    }

    std::string text;
    if (!unquote(raw, text) || text.find('\0') != std::string::npos) {
        return nullptr;
    }
    if (relative_to_home) {
        // "$HOME" + "/rest" (or nothing): strip the separator so operator/ appends.
        const std::size_t rest = text.find_first_not_of('/');
        value = rest == std::string::npos ? home : normalized(home / text.substr(rest));
    } else {
        value = normalized(Path(text));
    }
    return field;
}

} // namespace

Result<UserDirs> parse_user_dirs(std::string_view text, const Path& home) {
    try {
        if (home.empty() || !home.is_absolute()) {
            return fail(ErrorCode::InvalidArgument,
                        "parse_user_dirs: home must be absolute: '" + home.native() + "'");
        }
        const Path base = normalized(home);
        UserDirs dirs;
        bool desktop_set = false;
        while (!text.empty()) {
            const std::size_t nl = text.find('\n');
            const std::string_view line = text.substr(0, nl);
            text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
            Path value;
            if (Path UserDirs::*field = parse_line(line, base, value); field != nullptr) {
                dirs.*field = std::move(value);
                desktop_set = desktop_set || field == &UserDirs::desktop;
            }
        }
        if (!desktop_set) {
            dirs.desktop = base / "Desktop";
        }
        return dirs;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "parse_user_dirs: out of memory");
    }
}

Result<UserDirs> read_user_dirs(const Path& home, const Path& config_home) {
    try {
        if (home.empty() || !home.is_absolute()) {
            return fail(ErrorCode::InvalidArgument,
                        "read_user_dirs: home must be absolute: '" + home.native() + "'");
        }
        if (config_home.empty() || !config_home.is_absolute()) {
            return fail(ErrorCode::InvalidArgument,
                        "read_user_dirs: config_home must be absolute: '" + config_home.native() +
                            "'");
        }
        const Path file = config_home / "user-dirs.dirs";
        const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            const int err = errno;
            if (err == ENOENT || err == ENOTDIR) {
                return parse_user_dirs({}, home); // no file: defaults
            }
            return Unexpected<Error>(errno_error(err, "read_user_dirs: open", file));
        }
        // Read at most cap + 1 bytes: more than the cap means "too large", whatever fstat says
        // (the file may grow, or be a special file without a size).
        std::string text(kMaxUserDirsFile + 1, '\0');
        std::size_t used = 0;
        int read_errno = 0;
        while (used < text.size()) {
            const ssize_t got = ::read(fd, text.data() + used, text.size() - used);
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                read_errno = errno;
                break;
            }
            if (got == 0) {
                break;
            }
            used += static_cast<std::size_t>(got);
        }
        ::close(fd);
        if (read_errno != 0) {
            return Unexpected<Error>(errno_error(read_errno, "read_user_dirs: read", file));
        }
        if (used > kMaxUserDirsFile) {
            return parse_user_dirs({}, home); // too large: treated as empty
        }
        text.resize(used);
        return parse_user_dirs(text, home);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "read_user_dirs: out of memory");
    }
}

Result<Path> user_home(std::string_view user_name) {
    try {
        if (user_name.empty() || user_name.find('/') != std::string_view::npos ||
            user_name.find('\0') != std::string_view::npos) {
            return fail(ErrorCode::InvalidArgument,
                        "user_home: invalid user name '" + std::string(user_name) + "'");
        }
        const std::string name(user_name);
        long size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
        if (size <= 0) {
            size = 16384;
        }
        std::vector<char> buffer(static_cast<std::size_t>(size));
        passwd entry{};
        passwd* found = nullptr;
        int rc = 0;
        while ((rc = ::getpwnam_r(name.c_str(), &entry, buffer.data(), buffer.size(), &found)) ==
                   ERANGE &&
               buffer.size() < (1U << 20U)) {
            buffer.resize(buffer.size() * 2);
        }
        if (rc == 0 && found == nullptr) {
            return fail(ErrorCode::NotFound, "user_home: no such user '" + name + "'");
        }
        if (rc != 0) {
            // getpwnam_r(3): these mean "not found" on some NSS backends.
            if (rc == ENOENT || rc == ESRCH || rc == EBADF || rc == EPERM) {
                return fail(ErrorCode::NotFound, "user_home: no such user '" + name + "'");
            }
            if (rc == ENOMEM) {
                return fail(ErrorCode::OutOfMemory, "user_home: out of memory");
            }
            return fail(ErrorCode::PlatformError, "user_home: getpwnam_r('" + name +
                                                      "'): " + platform_internal::errno_text(rc));
        }
        if (found->pw_dir == nullptr || found->pw_dir[0] != '/') {
            return fail(ErrorCode::NotFound,
                        "user_home: user '" + name + "' has no absolute home directory");
        }
        return Path(found->pw_dir);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "user_home: out of memory");
    }
}

} // namespace navigator::platform
