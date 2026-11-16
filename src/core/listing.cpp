// list_directory() and stat() (navigator/fs.hpp). POSIX only: open(O_DIRECTORY) + fdopendir +
// readdir (getdents64 underneath) + fstatat relative to the directory fd. No std::filesystem
// iteration; std::filesystem::path is used only as the byte container of the argument.
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

namespace navigator::fs {
namespace {

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

// Errors of stat() and of per-entry metadata calls.
ErrorCode map_stat_errno(int err) noexcept {
    switch (err) {
    case ELOOP:
    case ENAMETOOLONG: return ErrorCode::InvalidArgument;
    case ENOENT:
    case ENOTDIR: return ErrorCode::NotFound;
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case ENOMEM: return ErrorCode::OutOfMemory;
    default: return ErrorCode::IoError;
    }
}

EntryKind kind_from_mode(mode_t mode) noexcept {
    switch (mode & S_IFMT) {
    case S_IFREG: return EntryKind::File;
    case S_IFDIR: return EntryKind::Directory;
    case S_IFLNK: return EntryKind::Symlink;
    default: return EntryKind::Other;
    }
}

EntryKind kind_from_dtype(unsigned char type) noexcept {
    switch (type) {
    case DT_REG: return EntryKind::File;
    case DT_DIR: return EntryKind::Directory;
    case DT_LNK: return EntryKind::Symlink;
    case DT_UNKNOWN: return EntryKind::Unknown;
    default: return EntryKind::Other;
    }
}

Timestamp to_timestamp(const struct timespec& ts) noexcept {
    using std::chrono::nanoseconds;
    constexpr std::int64_t kNs = 1'000'000'000;
    constexpr std::int64_t kMaxSec = std::numeric_limits<std::int64_t>::max() / kNs - 1;
    const auto sec = static_cast<std::int64_t>(ts.tv_sec);
    if (sec > kMaxSec) {
        return Timestamp{nanoseconds{std::numeric_limits<std::int64_t>::max()}};
    }
    if (sec < -kMaxSec) {
        return Timestamp{nanoseconds{std::numeric_limits<std::int64_t>::min()}};
    }
    return Timestamp{nanoseconds{sec * kNs + static_cast<std::int64_t>(ts.tv_nsec)}};
}

// Effective credentials, captured once per call.
class Credentials {
public:
    Credentials() : uid_(geteuid()), gid_(getegid()) {
        const int n = getgroups(0, nullptr);
        if (n > 0) {
            groups_.resize(static_cast<std::size_t>(n));
            const int got = getgroups(n, groups_.data());
            groups_.resize(got > 0 ? static_cast<std::size_t>(got) : 0U);
        }
    }

    [[nodiscard]] bool can_read(std::uint32_t mode, std::uint32_t uid,
                                std::uint32_t gid) const noexcept {
        if (uid_ == 0) {
            return true;
        }
        if (uid == uid_) {
            return (mode & S_IRUSR) != 0;
        }
        if (in_group(gid)) {
            return (mode & S_IRGRP) != 0;
        }
        return (mode & S_IROTH) != 0;
    }

private:
    [[nodiscard]] bool in_group(std::uint32_t gid) const noexcept {
        if (gid == gid_) {
            return true;
        }
        for (const gid_t g : groups_) {
            if (g == gid) {
                return true;
            }
        }
        return false;
    }

    uid_t uid_;
    gid_t gid_;
    std::vector<gid_t> groups_;
};

// Copies the metadata fields of `st` (whatever it describes) into `entry`.
void fill_metadata(DirEntry& entry, const struct stat& st, const Credentials& creds) noexcept {
    const mode_t type = st.st_mode & S_IFMT;
    entry.size = (type == S_IFREG || type == S_IFLNK) && st.st_size > 0
                     ? static_cast<std::uint64_t>(st.st_size)
                     : 0U;
    entry.mtime = to_timestamp(st.st_mtim);
    entry.mode = static_cast<std::uint32_t>(st.st_mode);
    entry.uid = static_cast<std::uint32_t>(st.st_uid);
    entry.gid = static_cast<std::uint32_t>(st.st_gid);
    entry.id = FileId{static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino)};
    entry.readable = creds.can_read(entry.mode, entry.uid, entry.gid);
}

// Fills everything except name/hidden from an lstat result `lst` of `name` relative to `dirfd`.
void describe(DirEntry& entry, int dirfd, const char* name, const struct stat& lst,
              bool follow_symlinks, const Credentials& creds) noexcept {
    entry.kind = kind_from_mode(lst.st_mode);
    if (entry.kind != EntryKind::Symlink) {
        fill_metadata(entry, lst, creds);
        return;
    }
    struct stat target {};
    if (fstatat(dirfd, name, &target, 0) == 0) {
        entry.target_kind = kind_from_mode(target.st_mode);
        fill_metadata(entry, follow_symlinks ? target : lst, creds);
        return;
    }
    const int err = errno;
    entry.dangling = err == ENOENT || err == ENOTDIR;
    entry.target_kind = EntryKind::Unknown;
    fill_metadata(entry, lst, creds); // dangling / unreadable target: lstat metadata
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
        describe(entry, dir_fd, cname, lst, options.follow_symlinks, creds);
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
        describe(entry, AT_FDCWD, path.c_str(), lst, follow_symlinks, creds);
        return entry;
    } catch (const std::bad_alloc&) {
        return out_of_memory();
    }
}

} // namespace navigator::fs
