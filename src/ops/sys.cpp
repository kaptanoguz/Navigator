// src/ops/sys.cpp: see sys.hpp.
#include "sys.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace navigator::ops::detail {

void Fd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

int Fd::close_checked() noexcept {
    if (fd_ < 0) {
        return 0;
    }
    const int rc = ::close(std::exchange(fd_, -1));
    // EINTR after close: the descriptor is released on Linux; treat as success.
    return rc == 0 || errno == EINTR ? 0 : errno;
}

ErrorCode code_from_errno(int err) noexcept {
    switch (err) {
    case 0: return ErrorCode::Unknown;
    case ENOENT:
    case ENOTDIR: return ErrorCode::NotFound;
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case EEXIST: return ErrorCode::AlreadyExists;
    case ENOSPC:
    case EDQUOT: return ErrorCode::NoSpace;
    case EROFS: return ErrorCode::ReadOnlyFilesystem;
    case EXDEV: return ErrorCode::CrossDevice;
    case ENOTEMPTY: return ErrorCode::NotEmpty;
    case ENOMEM: return ErrorCode::OutOfMemory;
    case ECANCELED: return ErrorCode::Cancelled;
    case EINVAL:
    case ENAMETOOLONG:
    case ELOOP:
    case EILSEQ:
    case EISDIR: return ErrorCode::InvalidArgument;
    case EOPNOTSUPP:
    case ENOSYS: return ErrorCode::Unsupported;
    default: return ErrorCode::IoError;
    }
}

Error errno_error(int err, std::string_view what, const Path& path) {
    std::array<char, 256> buf{};
    // GNU strerror_r returns a pointer that may or may not be `buf`.
    const char* text = ::strerror_r(err, buf.data(), buf.size());
    std::string msg(what);
    msg += " '";
    msg += path.native();
    msg += "': ";
    msg += text;
    return Error{code_from_errno(err), std::move(msg)};
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

Timestamp to_timestamp(const struct timespec& ts) noexcept {
    return Timestamp{std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec}};
}

FileId id_of(const struct stat& st) noexcept {
    return FileId{static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino)};
}

ItemGuard guard_from(const Path& path, const struct stat& st) {
    ItemGuard g;
    g.path = path;
    g.kind = kind_of(st.st_mode);
    g.id = id_of(st);
    g.size = S_ISDIR(st.st_mode) ? 0 : static_cast<std::uint64_t>(st.st_size);
    g.mtime = to_timestamp(st.st_mtim);
    return g;
}

ConflictSide side_from(const Path& path, const struct stat& st) {
    ConflictSide s;
    s.path = path;
    s.kind = kind_of(st.st_mode);
    s.size = S_ISDIR(st.st_mode) ? 0 : static_cast<std::uint64_t>(st.st_size);
    s.mtime = to_timestamp(st.st_mtim);
    s.mode = static_cast<std::uint32_t>(st.st_mode);
    return s;
}

bool matches_guard(const struct stat& st, const ItemGuard& guard) noexcept {
    if (kind_of(st.st_mode) != guard.kind || !(id_of(st) == guard.id)) {
        return false;
    }
    if (!S_ISDIR(st.st_mode) && static_cast<std::uint64_t>(st.st_size) != guard.size) {
        return false;
    }
    return to_timestamp(st.st_mtim) == guard.mtime;
}

bool same_inode(const struct stat& a, const struct stat& b) noexcept {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

bool same_inode(const struct stat& st, const FileId& id) noexcept { return id_of(st) == id; }

int lstat_at(int dirfd, const std::string& name, struct stat& st) noexcept {
    return ::fstatat(dirfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : errno;
}

int lstat_path(const Path& path, struct stat& st) noexcept {
    return ::lstat(path.c_str(), &st) == 0 ? 0 : errno;
}

int open_dir_path(const Path& path) noexcept {
    return ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

int open_dir_at(int dirfd, const std::string& name) noexcept {
    return ::openat(dirfd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

int rename_noreplace(int olddir, const std::string& oldname, int newdir,
                     const std::string& newname) noexcept {
    return ::renameat2(olddir, oldname.c_str(), newdir, newname.c_str(), RENAME_NOREPLACE) == 0
               ? 0
               : errno;
}

int rename_no_overwrite(int olddir, const std::string& oldname, int newdir,
                        const std::string& newname, bool allow_link) noexcept {
    const int rc = rename_noreplace(olddir, oldname, newdir, newname);
    if (rc != EINVAL && rc != ENOSYS) {
        return rc;
    }
    // RENAME_NOREPLACE unsupported (some FUSE / NFS): link(2) fails atomically with EEXIST.
    if (allow_link) {
        if (::linkat(olddir, oldname.c_str(), newdir, newname.c_str(), 0) == 0) {
            ::unlinkat(olddir, oldname.c_str(), 0); // the data lives on under the new name
            return 0;
        }
        const int lerr = errno;
        if (lerr == EEXIST) {
            return EEXIST;
        }
        if (lerr != EPERM && lerr != EOPNOTSUPP && lerr != EMLINK && lerr != ENOSYS) {
            return lerr;
        }
    }
    // Last resort (vfat / exfat without hard links): check, then rename. A program creating the
    // same name between the two calls loses that file; documented in docs/architecture.md.
    struct stat st {};
    if (::fstatat(newdir, newname.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        return EEXIST;
    }
    if (errno != ENOENT) {
        return errno;
    }
    return ::renameat(olddir, oldname.c_str(), newdir, newname.c_str()) == 0 ? 0 : errno;
}

std::string random_token() {
    std::array<unsigned char, 6> bytes{};
    std::size_t got = 0;
    while (got < bytes.size()) {
        const ssize_t n = ::getrandom(bytes.data() + got, bytes.size() - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            // Fallback: address and clock entropy; uniqueness is still enforced by O_EXCL.
            static std::atomic<std::uint64_t> counter{0};
            std::uint64_t v = counter.fetch_add(1) ^ static_cast<std::uint64_t>(::getpid()) ^
                              static_cast<std::uint64_t>(
                                  std::chrono::steady_clock::now().time_since_epoch().count());
            for (auto& b : bytes) {
                b = static_cast<unsigned char>(v & 0xFFU);
                v >>= 8U;
            }
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char b : bytes) {
        out.push_back(kHex[b >> 4U]);
        out.push_back(kHex[b & 0x0FU]);
    }
    return out;
}

std::string_view utf8_prefix(std::string_view text, std::size_t max_bytes) noexcept {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    // Back off over at most 3 continuation bytes so a multi-byte sequence is not split.
    for (int i = 0; i < 3 && cut > 0; ++i) {
        const auto c = static_cast<unsigned char>(text[cut]);
        if ((c & 0xC0U) != 0x80U) {
            break;
        }
        --cut;
    }
    return text.substr(0, cut);
}

std::string temp_name_for(std::string_view final_name) {
    const std::string token = random_token();
    // "." + name + ".navigator-" + token + ".part"
    const std::size_t fixed = 1 + 11 + token.size() + 5;
    const std::string_view name = utf8_prefix(final_name, kNameMaxBytes - fixed);
    std::string out;
    out.reserve(fixed + name.size());
    out += '.';
    out += name;
    out += ".navigator-";
    out += token;
    out += ".part";
    return out;
}

mode_t process_umask() noexcept {
    static const mode_t mask = [] {
        mode_t m = 022;
        std::FILE* f = std::fopen("/proc/self/status", "re");
        if (f == nullptr) {
            return m;
        }
        std::array<char, 256> line{};
        while (std::fgets(line.data(), static_cast<int>(line.size()), f) != nullptr) {
            if (std::strncmp(line.data(), "Umask:", 6) == 0) {
                m = static_cast<mode_t>(std::strtoul(line.data() + 6, nullptr, 8) & 0777U);
                break;
            }
        }
        std::fclose(f);
        return m;
    }();
    return mask;
}

Result<Path> canonical_entry(const Path& path) {
    const Path parent = path.parent_path();
    const Path name = path.filename();
    if (parent.empty() || name.empty()) {
        return path; // "/" itself
    }
    std::unique_ptr<char, decltype(&std::free)> real(::realpath(parent.c_str(), nullptr),
                                                     &std::free);
    if (!real) {
        return Unexpected<Error>(errno_error(errno, "cannot resolve", parent));
    }
    return Path(real.get()) / name;
}

int read_dir_names(int dirfd, std::vector<std::string>& names) {
    const int dupfd = ::fcntl(dirfd, F_DUPFD_CLOEXEC, 0);
    if (dupfd < 0) {
        return errno;
    }
    DIR* dir = ::fdopendir(dupfd);
    if (dir == nullptr) {
        const int err = errno;
        ::close(dupfd);
        return err;
    }
    ::rewinddir(dir);
    int err = 0;
    for (;;) {
        errno = 0;
        const dirent* ent = ::readdir(dir);
        if (ent == nullptr) {
            err = errno;
            break;
        }
        const std::string_view n(ent->d_name);
        if (n == "." || n == "..") {
            continue;
        }
        names.emplace_back(n);
    }
    ::closedir(dir);
    return err;
}

bool is_normal_absolute(const Path& path) {
    const std::string& s = path.native();
    if (s.empty() || s.front() != '/') {
        return false;
    }
    if (s == "/") {
        return true;
    }
    if (s.back() == '/') {
        return false;
    }
    std::size_t pos = 1;
    while (pos <= s.size()) {
        std::size_t next = s.find('/', pos);
        if (next == std::string::npos) {
            next = s.size();
        }
        const std::string_view comp(s.data() + pos, next - pos);
        if (comp.empty() || comp == "." || comp == "..") {
            return false;
        }
        pos = next + 1;
    }
    return true;
}

namespace {

std::mutex& seam_mutex() {
    static std::mutex m;
    return m;
}
IoFaultHook& io_hook_storage() {
    static IoFaultHook hook;
    return hook;
}
FreeSpaceHook& space_hook_storage() {
    static FreeSpaceHook hook;
    return hook;
}
std::atomic<bool> g_io_hook_set{false};
std::atomic<bool> g_space_hook_set{false};

} // namespace

void set_io_fault_hook(IoFaultHook hook) {
    const std::lock_guard lock(seam_mutex());
    g_io_hook_set.store(static_cast<bool>(hook));
    io_hook_storage() = std::move(hook);
}

int io_fault(IoSite site, const Path& path, std::uint64_t offset) {
    if (!g_io_hook_set.load(std::memory_order_relaxed)) {
        return 0;
    }
    IoFaultHook hook;
    {
        const std::lock_guard lock(seam_mutex());
        hook = io_hook_storage();
    }
    return hook ? hook(site, path, offset) : 0;
}

void set_free_space_hook(FreeSpaceHook hook) {
    const std::lock_guard lock(seam_mutex());
    g_space_hook_set.store(static_cast<bool>(hook));
    space_hook_storage() = std::move(hook);
}

std::optional<std::uint64_t> available_bytes(const Path& dir) {
    if (g_space_hook_set.load(std::memory_order_relaxed)) {
        FreeSpaceHook hook;
        {
            const std::lock_guard lock(seam_mutex());
            hook = space_hook_storage();
        }
        if (hook) {
            if (auto v = hook(dir)) {
                return v;
            }
        }
    }
    struct statvfs vfs {};
    if (::statvfs(dir.c_str(), &vfs) != 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(vfs.f_bavail) * static_cast<std::uint64_t>(vfs.f_frsize);
}

} // namespace navigator::ops::detail
