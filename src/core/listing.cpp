// list_directory() and stat() (navigator/fs.hpp). POSIX only: open(O_DIRECTORY) + fdopendir +
// readdir (getdents64 underneath) + fstatat relative to the directory fd. No std::filesystem
// iteration; std::filesystem::path is used only as the byte container of the argument.
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <new>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <navigator/fs.hpp>

#include "entry_meta.hpp"

namespace navigator::fs {
namespace {

using core::Credentials;
using core::kind_from_dtype;
using core::map_stat_errno;

// stop is polled when (entries read & kStopMask) == 0, i.e. every 256 readdir results; the
// contract requires at least every 1024.
constexpr std::size_t kStopMask = 255;

// strerror_r has a GNU (char*) and an XSI (int) variant; accept both.
[[maybe_unused]] const char* strerror_pick(int rc, const char* buf) noexcept {
    return rc == 0 ? buf : "unknown error";
}
[[maybe_unused]] const char* strerror_pick(const char* rc, const char* /*buf*/) noexcept {
    return rc;
}

std::string errno_message(std::string_view what, const Path& path, int err) {
    char buf[256];
    buf[0] = '\0';
    const char* text = strerror_pick(strerror_r(err, buf, sizeof buf), buf);
    std::string msg;
    msg.append(what).append(" '").append(path.native()).append("': ").append(text);
    msg.append(" (errno ").append(std::to_string(err)).append(")");
    return msg;
}

// Errors of the directory open/read in list_directory().
ErrorCode map_list_errno(int err) noexcept {
    switch (err) {
    case ENOTDIR:
    case ELOOP:
    case ENAMETOOLONG: return ErrorCode::InvalidArgument;
    case ENOENT: return ErrorCode::NotFound;
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case ENOMEM: return ErrorCode::OutOfMemory;
    default: return ErrorCode::IoError;
    }
}

class DirHandle {
public:
    explicit DirHandle(DIR* dir) noexcept : dir_(dir) {}
    DirHandle(const DirHandle&) = delete;
    DirHandle& operator=(const DirHandle&) = delete;
    DirHandle(DirHandle&&) = delete;
    DirHandle& operator=(DirHandle&&) = delete;
    ~DirHandle() {
        if (dir_ != nullptr) {
            closedir(dir_);
        }
    }
    [[nodiscard]] DIR* get() const noexcept { return dir_; }

private:
    DIR* dir_;
};

Unexpected<Error> cancelled() { return fail(ErrorCode::Cancelled, "listing cancelled"); }

Unexpected<Error> out_of_memory() { return fail(ErrorCode::OutOfMemory, "out of memory"); }

Result<std::vector<DirEntry>> list_impl(const Path& dir, const ListOptions& options,
                                        const std::stop_token& stop) {
    if (dir.empty()) {
        return fail(ErrorCode::InvalidArgument, "list_directory: empty path");
    }
    if (stop.stop_requested()) {
        return cancelled();
    }
    const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        return fail(map_list_errno(err), errno_message("list_directory: open", dir, err));
    }
    DIR* raw = fdopendir(fd);
    if (raw == nullptr) {
        const int err = errno;
        close(fd);
        return fail(map_list_errno(err), errno_message("list_directory: fdopendir", dir, err));
    }
    const DirHandle handle(raw);
    const int dir_fd = dirfd(raw);
    const Credentials creds;

    std::vector<DirEntry> entries;
    std::size_t seen = 0;
    for (;;) {
        if ((seen++ & kStopMask) == 0 && stop.stop_requested()) {
            return cancelled();
        }
        errno = 0;
        const struct dirent* d = readdir(raw);
        if (d == nullptr) {
            const int err = errno;
            if (err != 0) {
                return fail(map_list_errno(err), errno_message("list_directory: read", dir, err));
            }
            break;
        }
        const char* cname = d->d_name;
        const std::string_view name(cname);
        if (name == "." || name == "..") {
            continue;
        }
        const bool hidden = is_hidden_name(name);
        if (hidden && !options.show_hidden) {
            continue; // filtered before any stat
        }
        DirEntry& entry = entries.emplace_back();
        entry.name.assign(name);
        entry.hidden = hidden;
        struct stat lst {};
        if (fstatat(dir_fd, cname, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
            entry.metadata_error = map_stat_errno(errno);
            entry.kind = kind_from_dtype(d->d_type);
            continue;
        }
        core::describe_entry(entry, dir_fd, cname, lst, options.follow_symlinks, creds);
    }
    if (stop.stop_requested()) {
        return cancelled();
    }
    return entries;
}

// Last non-empty component: "/a/b/" -> "b", "/" and "//" -> "/".
std::string last_component(const std::string& p) {
    std::size_t end = p.size();
    while (end > 0 && p[end - 1] == '/') {
        --end;
    }
    if (end == 0) {
        return "/";
    }
    const std::size_t slash = p.rfind('/', end - 1);
    const std::size_t begin = slash == std::string::npos ? 0 : slash + 1;
    return p.substr(begin, end - begin);
}

} // namespace

Result<std::vector<DirEntry>> list_directory(const Path& dir, const ListOptions& options) {
    try {
        return list_impl(dir, options, std::stop_token{});
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

// The by-value std::stop_token parameter is fixed by fs.hpp.
// NOLINTBEGIN(performance-unnecessary-value-param)
Result<std::vector<DirEntry>> list_directory(const Path& dir, const ListOptions& options,
                                             std::stop_token stop) {
    // NOLINTEND(performance-unnecessary-value-param)
    try {
        return list_impl(dir, options, stop);
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

Result<DirEntry> stat(const Path& path, bool follow_symlinks) {
    try {
        if (path.empty()) {
            return fail(ErrorCode::InvalidArgument, "stat: empty path");
        }
        struct stat lst {};
        if (fstatat(AT_FDCWD, path.c_str(), &lst, AT_SYMLINK_NOFOLLOW) != 0) {
            const int err = errno;
            return fail(map_stat_errno(err), errno_message("stat", path, err));
        }
        const Credentials creds;
        DirEntry entry;
        entry.name = last_component(path.native());
        entry.hidden = is_hidden_name(entry.name);
        core::describe_entry(entry, AT_FDCWD, path.c_str(), lst, follow_symlinks, creds);
        return entry;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

} // namespace navigator::fs
