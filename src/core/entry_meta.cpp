// Per-entry metadata shared by list_directory() and Walker (see entry_meta.hpp).
#include "entry_meta.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace navigator::core {
namespace {

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

// Copies the metadata fields of `st` (whatever it describes) into `entry`.
void fill_metadata(fs::DirEntry& entry, const struct stat& st, const Credentials& creds) noexcept {
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

} // namespace

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

fs::EntryKind kind_from_mode(mode_t mode) noexcept {
    switch (mode & S_IFMT) {
    case S_IFREG: return fs::EntryKind::File;
    case S_IFDIR: return fs::EntryKind::Directory;
    case S_IFLNK: return fs::EntryKind::Symlink;
    default: return fs::EntryKind::Other;
    }
}

fs::EntryKind kind_from_dtype(unsigned char type) noexcept {
    switch (type) {
    case DT_REG: return fs::EntryKind::File;
    case DT_DIR: return fs::EntryKind::Directory;
    case DT_LNK: return fs::EntryKind::Symlink;
    case DT_UNKNOWN: return fs::EntryKind::Unknown;
    default: return fs::EntryKind::Other;
    }
}

Credentials::Credentials() : uid_(geteuid()), gid_(getegid()) {
    const int n = getgroups(0, nullptr);
    if (n > 0) {
        groups_.resize(static_cast<std::size_t>(n));
        const int got = getgroups(n, groups_.data());
        groups_.resize(got > 0 ? static_cast<std::size_t>(got) : 0U);
    }
}

bool Credentials::can_read(std::uint32_t mode, std::uint32_t uid,
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

bool Credentials::in_group(std::uint32_t gid) const noexcept {
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

void describe_entry(fs::DirEntry& entry, int dirfd, const char* name, const struct stat& lst,
                    bool follow_symlinks, const Credentials& creds) noexcept {
    entry.kind = kind_from_mode(lst.st_mode);
    if (entry.kind != fs::EntryKind::Symlink) {
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
    entry.target_kind = fs::EntryKind::Unknown;
    fill_metadata(entry, lst, creds); // dangling / unreadable target: lstat metadata
}

} // namespace navigator::core
