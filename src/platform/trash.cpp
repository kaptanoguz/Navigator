// <navigator/trash.hpp>: the FreeDesktop.org Trash 1.0 service (rules T1..T7). Same-filesystem
// rename only; names reserved with O_EXCL; per-volume trashes checked for sticky bit, symlinks
// and ownership; never copies, never deletes item data. libc/Linux only (no GIO).
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <navigator/platform.hpp>
#include <navigator/trash.hpp>

#include "errno_util.hpp"
#include "trash_internal.hpp"

namespace navigator::trash {

namespace ti = trash_internal;
using platform_internal::ops_errno_code;
using platform_internal::ops_errno_error;

namespace {

constexpr unsigned kMaxCandidates = 100000;
constexpr off_t kMaxInfoBytes = off_t{64} * 1024;

// Owned file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_ = -1;
};

[[nodiscard]] fs::EntryKind kind_of(mode_t mode) noexcept {
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

[[nodiscard]] Timestamp mtime_of(const struct stat& st) noexcept {
    return Timestamp{std::chrono::seconds{st.st_mtim.tv_sec} +
                     std::chrono::nanoseconds{st.st_mtim.tv_nsec}};
}

void fill_item_metadata(TrashedItem& item, const struct stat& st) {
    item.kind = kind_of(st.st_mode);
    item.size = S_ISDIR(st.st_mode) ? 0U : static_cast<std::uint64_t>(st.st_size);
    item.mtime = mtime_of(st);
    item.id = FileId{static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino)};
}

// lstat errno -> error; ENOTDIR (a component is a file) means "does not exist".
[[nodiscard]] Error lookup_error(int err, std::string_view what, const Path& path) {
    Error e = ops_errno_error(err, what, path);
    if (err == ENOTDIR) {
        e.code = ErrorCode::NotFound;
    }
    return e;
}

// st_dev of `path` or of its nearest existing ancestor (a trash that does not exist yet will be
// created on that filesystem). 0 when nothing can be stat'ed.
[[nodiscard]] std::uint64_t nearest_device(const Path& path) {
    Path p = path;
    while (true) {
        struct stat st {};
        if (::stat(p.c_str(), &st) == 0) {
            return static_cast<std::uint64_t>(st.st_dev);
        }
        if (!p.has_relative_path()) {
            return 0;
        }
        p = p.parent_path();
    }
}

// Nearest existing ancestor of `path` (or `path` itself).
[[nodiscard]] Path nearest_existing(const Path& path) {
    Path p = path;
    while (true) {
        struct stat st {};
        if (::stat(p.c_str(), &st) == 0 || !p.has_relative_path()) {
            return p;
        }
        p = p.parent_path();
    }
}

// mkdir -p with mode 0700 for every missing component (GIO's g_mkdir_with_parents(.., 0700)).
[[nodiscard]] Result<void> make_dirs_0700(const Path& path) {
    std::vector<Path> missing;
    Path p = path;
    while (true) {
        struct stat st {};
        if (::stat(p.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                return fail(ErrorCode::InvalidArgument,
                            "trash: '" + p.native() + "' is not a directory");
            }
            break;
        }
        if (errno != ENOENT) {
            return Unexpected<Error>(ops_errno_error(errno, "trash: stat", p));
        }
        missing.push_back(p);
        if (!p.has_relative_path()) {
            break;
        }
        p = p.parent_path();
    }
    std::reverse(missing.begin(), missing.end()); // outermost first
    for (const Path& dir : missing) {
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            return Unexpected<Error>(ops_errno_error(errno, "trash: mkdir", dir));
        }
    }
    return ok();
}

// Opens (creating 0700 when missing) the subdirectory `name` of `root_fd`, never following a
// symlink.
[[nodiscard]] Result<Fd> open_subdir(int root_fd, const char* name, const Path& for_message,
                                     bool create) {
    if (create && ::mkdirat(root_fd, name, 0700) != 0 && errno != EEXIST) {
        return Unexpected<Error>(ops_errno_error(errno, "trash: mkdir", for_message / name));
    }
    const int fd = ::openat(root_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        Error e = ops_errno_error(errno, "trash: open", for_message / name);
        if (errno == ELOOP || errno == ENOTDIR) {
            e.code = ErrorCode::Unsupported; // files/ or info/ is a symlink or not a directory
        }
        return Unexpected<Error>(std::move(e));
    }
    return Fd(fd);
}

[[nodiscard]] bool write_all(int fd, std::string_view data) noexcept {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

// renameat2(RENAME_NOREPLACE); on a filesystem without it, a check-then-rename fallback.
[[nodiscard]] int rename_noreplace(int olddirfd, const char* oldpath, int newdirfd,
                                   const char* newpath) noexcept {
    if (::renameat2(olddirfd, oldpath, newdirfd, newpath, RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) {
        return -1;
    }
    struct stat st {};
    if (::fstatat(newdirfd, newpath, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    return ::renameat(olddirfd, oldpath, newdirfd, newpath);
}

[[nodiscard]] LocalDateTime system_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    if (::localtime_r(&t, &tm) == nullptr) {
        return {};
    }
    LocalDateTime out;
    out.year = tm.tm_year + 1900;
    out.month = tm.tm_mon + 1;
    out.day = tm.tm_mday;
    out.hour = tm.tm_hour;
    out.minute = tm.tm_min;
    out.second = tm.tm_sec;
    return out;
}

// Owner / type checks of an existing trash directory (T4). Returns true when usable.
[[nodiscard]] bool valid_owned_dir(const struct stat& st, std::uint32_t uid) noexcept {
    return S_ISDIR(st.st_mode) && st.st_uid == uid;
}

[[nodiscard]] std::uint64_t device_of(const Path& p) noexcept {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0U;
}

[[nodiscard]] bool writable_dir(const Path& p) noexcept {
    return ::access(p.c_str(), W_OK | X_OK) == 0;
}

[[nodiscard]] bool is_trashinfo_name(std::string_view name) noexcept {
    constexpr std::string_view kSuffix = ".trashinfo";
    return name.size() > kSuffix.size() && name.ends_with(kSuffix);
}

// Reads a small regular file below `dir_fd` without following a symlink.
[[nodiscard]] std::optional<std::string> read_small_file(int dir_fd, const char* name) {
    Fd fd(::openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!fd.valid()) {
        return std::nullopt;
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > kMaxInfoBytes) {
        return std::nullopt;
    }
    std::string out;
    out.resize(static_cast<std::size_t>(st.st_size) + 1);
    std::size_t got = 0;
    while (got < out.size()) {
        const ssize_t n = ::read(fd.get(), out.data() + got, out.size() - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        if (n == 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    if (got > static_cast<std::size_t>(kMaxInfoBytes)) {
        return std::nullopt;
    }
    out.resize(got);
    return out;
}

// Entry names of a directory fd (the fd itself stays open and owned by the caller).
[[nodiscard]] Result<std::vector<std::string>> read_names(int dir_fd, const Path& for_message) {
    const int dup_fd = ::fcntl(dir_fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        return Unexpected<Error>(ops_errno_error(errno, "trash: dup", for_message));
    }
    DIR* dir = ::fdopendir(dup_fd);
    if (dir == nullptr) {
        const int err = errno;
        ::close(dup_fd);
        return Unexpected<Error>(ops_errno_error(err, "trash: fdopendir", for_message));
    }
    ::rewinddir(dir);
    std::vector<std::string> names;
    while (true) {
        errno = 0;
        const dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            const int err = errno;
            ::closedir(dir);
            if (err != 0) {
                return Unexpected<Error>(ops_errno_error(err, "trash: readdir", for_message));
            }
            return names;
        }
        const std::string_view name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        names.emplace_back(name);
    }
}

// =============================================================================================
// The service
// =============================================================================================

struct Prepared {
    Path item;      // lexically normal path as given (what is renamed)
    Path real;      // realpath(parent) / name (topdir lookup, relative Path=)
    struct stat st; // lstat of the item
    TrashDir dir;
};

class Trash final : public ITrash {
public:
    Trash(Path home, std::uint32_t uid, VolumeQuery volumes, bool topdirs,
          std::function<LocalDateTime()> now)
        : home_(std::move(home)), uid_(uid), uid_text_(std::to_string(uid)),
          volumes_(std::move(volumes)), topdirs_(topdirs), now_(std::move(now)) {
        volumes_.query_space = false;
    }

    Result<TrashDir> trash_dir_for(const Path& path) override {
        try {
            auto prep = prepare(path, false);
            if (!prep) {
                return Unexpected<Error>(std::move(prep.error()));
            }
            if (prep->dir.kind == TrashDirKind::Home) {
                const Path existing = nearest_existing(home_);
                if (!writable_dir(existing)) {
                    return Unexpected<Error>(
                        ops_errno_error(errno, "trash: home trash not writable", existing));
                }
            }
            return std::move(prep->dir);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "trash_dir_for: allocation failed");
        }
    }

    Result<TrashedItem> trash(const Path& path) override {
        try {
            return trash_impl(path);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "trash: allocation failed");
        }
    }

    Result<std::vector<TrashDir>> trash_dirs() override {
        try {
            return trash_dirs_impl();
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "trash_dirs: allocation failed");
        }
    }

    Result<TrashListing> list(std::stop_token stop) override {
        try {
            return list_impl(stop);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "trash list: allocation failed");
        }
    }

    Result<void> restore(const TrashedItem& item, const Path& destination) override {
        try {
            return restore_impl(item, destination);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "trash restore: allocation failed");
        }
    }

    Result<void> forget(const TrashedItem& item) override {
        try {
            const Path files = item.files_path();
            struct stat st {};
            if (::lstat(files.c_str(), &st) == 0) {
                return fail(ErrorCode::InvalidArgument,
                            "trash forget: '" + files.native() + "' still exists");
            }
            if (errno != ENOENT && errno != ENOTDIR) {
                return Unexpected<Error>(ops_errno_error(errno, "trash forget: lstat", files));
            }
            const Path info = item.info_path();
            if (::unlink(info.c_str()) != 0) {
                return Unexpected<Error>(lookup_error(errno, "trash forget: unlink", info));
            }
            return ok();
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "trash forget: allocation failed");
        }
    }

    bool is_inside_trash(const Path& path) const override {
        try {
            if (path.empty()) {
                return false;
            }
            const Path n = ti::normal_path(path);
            if (ti::at_or_below(n, home_)) {
                return true;
            }
            const std::string user_name = ".Trash-" + uid_text_;
            bool prev_shared = false;
            for (const Path& part : n) {
                const std::string& s = part.native();
                if (s == user_name || (prev_shared && s == uid_text_)) {
                    return true;
                }
                prev_shared = s == ".Trash";
            }
            return false;
        } catch (...) {
            return true; // conservative: refuse rather than trash a trash
        }
    }

private:
    [[nodiscard]] TrashDir home_dir() const {
        return TrashDir{TrashDirKind::Home, home_, Path{}, nearest_device(home_)};
    }

    // T7 checks, lstat, filesystem and trash directory choice (T1, T3, T4).
    [[nodiscard]] Result<Prepared> prepare(const Path& path, bool create) const {
        if (path.empty() || !path.is_absolute()) {
            return fail(ErrorCode::InvalidArgument, "trash: empty or relative path");
        }
        Prepared prep{};
        prep.item = ti::normal_path(path);
        if (prep.item == Path("/") || !prep.item.has_relative_path()) {
            return fail(ErrorCode::InvalidArgument, "trash: refusing to trash '/'");
        }
        if (is_inside_trash(prep.item) || ti::at_or_below(home_, prep.item)) {
            return fail(ErrorCode::InvalidArgument,
                        "trash: '" + prep.item.native() + "' is a trash directory or inside one");
        }
        if (::lstat(prep.item.c_str(), &prep.st) != 0) {
            return Unexpected<Error>(lookup_error(errno, "trash: lstat", prep.item));
        }
        const Path parent = prep.item.parent_path();
        struct stat parent_st {};
        if (::stat(parent.c_str(), &parent_st) != 0) {
            return Unexpected<Error>(lookup_error(errno, "trash: stat", parent));
        }
        if (parent_st.st_dev != prep.st.st_dev) {
            return fail(ErrorCode::InvalidArgument,
                        "trash: '" + prep.item.native() + "' is a mount point");
        }
        std::error_code ec;
        const Path real_parent = std::filesystem::canonical(parent, ec);
        if (ec) {
            return Unexpected<Error>(lookup_error(ec.value(), "trash: realpath", parent));
        }
        prep.real = real_parent / prep.item.filename();
        const Path home_real = ti::normal_path(std::filesystem::weakly_canonical(home_, ec));
        if (is_inside_trash(prep.real) || (!ec && ti::at_or_below(home_real, prep.real))) {
            return fail(ErrorCode::InvalidArgument,
                        "trash: '" + prep.real.native() + "' is a trash directory or inside one");
        }
        struct statvfs vfs {};
        if (::statvfs(real_parent.c_str(), &vfs) == 0 && (vfs.f_flag & ST_RDONLY) != 0) {
            return fail(ErrorCode::ReadOnlyFilesystem,
                        "trash: '" + real_parent.native() + "' is on a read-only filesystem");
        }
        const auto item_dev = static_cast<std::uint64_t>(prep.st.st_dev);
        if (nearest_device(home_) == item_dev) {
            prep.dir = home_dir();
            return prep;
        }
        if (!topdirs_) {
            return fail(ErrorCode::Unsupported,
                        "trash: '" + prep.item.native() +
                            "' is not on the home trash's filesystem (topdir trashes disabled)");
        }
        // A directory is looked up by itself so that a mount point is recognised as one (T7);
        // anything else (a symlink above all) by its parent, never followed.
        const Path& lookup = S_ISDIR(prep.st.st_mode) ? prep.real : real_parent;
        auto volume = volume_for(lookup, volumes_);
        if (!volume) {
            if (volume.error().code == ErrorCode::NotFound) {
                return fail(ErrorCode::Unsupported,
                            "trash: no mount found for '" + lookup.native() + "'");
            }
            return Unexpected<Error>(std::move(volume.error()));
        }
        if (volume->read_only) {
            return fail(ErrorCode::ReadOnlyFilesystem,
                        "trash: '" + volume->mount_point.native() + "' is mounted read-only");
        }
        const Path topdir = ti::normal_path(volume->mount_point);
        if (prep.real == topdir) {
            return fail(ErrorCode::InvalidArgument,
                        "trash: '" + prep.item.native() + "' is a mount point");
        }
        if (device_of(topdir) != item_dev) {
            return fail(ErrorCode::Unsupported, "trash: mount point '" + topdir.native() +
                                                    "' is not on the item's filesystem");
        }
        auto dir = ti::select_topdir_trash(topdir, uid_, create);
        if (!dir) {
            return Unexpected<Error>(std::move(dir.error()));
        }
        if (ti::at_or_below(dir->root, prep.real)) {
            return fail(ErrorCode::InvalidArgument,
                        "trash: '" + prep.item.native() + "' contains its own trash");
        }
        prep.dir = std::move(*dir);
        return prep;
    }

    [[nodiscard]] Result<TrashedItem> trash_impl(const Path& path) const {
        auto prep_r = prepare(path, true);
        if (!prep_r) {
            return Unexpected<Error>(std::move(prep_r.error()));
        }
        Prepared& prep = *prep_r;
        const bool home = prep.dir.kind == TrashDirKind::Home;
        if (home) {
            if (auto made = make_dirs_0700(home_); !made) {
                return Unexpected<Error>(std::move(made.error()));
            }
        }
        Fd root(::open(prep.dir.root.c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | (home ? 0 : O_NOFOLLOW)));
        if (!root.valid()) {
            return Unexpected<Error>(ops_errno_error(errno, "trash: open", prep.dir.root));
        }
        auto files = open_subdir(root.get(), "files", prep.dir.root, true);
        if (!files) {
            return Unexpected<Error>(std::move(files.error()));
        }
        auto info = open_subdir(root.get(), "info", prep.dir.root, true);
        if (!info) {
            return Unexpected<Error>(std::move(info.error()));
        }
        struct stat files_st {};
        if (::fstat(files->get(), &files_st) != 0) {
            return Unexpected<Error>(ops_errno_error(errno, "trash: fstat", prep.dir.files_dir()));
        }
        if (files_st.st_dev != prep.st.st_dev) {
            return fail(ErrorCode::Unsupported, "trash: '" + prep.dir.files_dir().native() +
                                                    "' is on another filesystem (T1)");
        }
        prep.dir.device = device_of(prep.dir.root); // as trash_dirs() reports it

        const Path& original = home ? prep.item : prep.real;
        auto info_value = info_path_for(original, prep.dir.topdir);
        if (!info_value) {
            return Unexpected<Error>(std::move(info_value.error()));
        }
        TrashInfo ti_value;
        ti_value.path = *info_value;
        ti_value.deletion_date = now_ ? now_() : system_now();
        const std::string text = format_trashinfo(ti_value);
        const std::string base = prep.item.filename().native();

        for (unsigned n = 1; n <= kMaxCandidates; ++n) {
            const std::string name = ti::candidate_name(base, n);
            const std::string info_name = name + ".trashinfo";
            Fd fd(::openat(info->get(), info_name.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            if (!fd.valid()) {
                if (errno == EEXIST) {
                    continue;
                }
                return Unexpected<Error>(
                    ops_errno_error(errno, "trash: create", prep.dir.info_dir() / info_name));
            }
            const bool written = write_all(fd.get(), text);
            const int write_err = errno;
            const int close_rc = ::close(fd.release());
            if (!written || close_rc != 0) {
                const int err = written ? errno : write_err;
                (void)::unlinkat(info->get(), info_name.c_str(), 0);
                return Unexpected<Error>(
                    ops_errno_error(err, "trash: write", prep.dir.info_dir() / info_name));
            }
            if (rename_noreplace(AT_FDCWD, prep.item.c_str(), files->get(), name.c_str()) != 0) {
                const int err = errno;
                (void)::unlinkat(info->get(), info_name.c_str(), 0);
                if (err == EEXIST || err == ENOTEMPTY) {
                    continue; // files/<name> exists without an info file (orphan): next name
                }
                Error e = ops_errno_error(err, "trash: rename", prep.item);
                if (err == EXDEV) {
                    e.code = ErrorCode::Unsupported;
                } else if (err == ENOTDIR) {
                    e.code = ErrorCode::NotFound;
                }
                return Unexpected<Error>(std::move(e));
            }
            TrashedItem item;
            item.dir = prep.dir;
            item.name = name;
            auto orig = original_path_from(*info_value, prep.dir.topdir);
            item.original_path = orig ? *orig : original;
            item.deletion_date = ti_value.deletion_date;
            struct stat st {};
            if (::fstatat(files->get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
                fill_item_metadata(item, st);
            } else {
                fill_item_metadata(item, prep.st);
            }
            return item;
        }
        return fail(ErrorCode::IoError,
                    "trash: no free name for '" + base + "' in " + prep.dir.root.native());
    }

    [[nodiscard]] Result<std::vector<TrashDir>> trash_dirs_impl() const {
        std::vector<TrashDir> dirs;
        dirs.push_back(home_dir());
        if (!topdirs_) {
            return dirs;
        }
        auto volumes = list_volumes(volumes_);
        if (!volumes) {
            return Unexpected<Error>(std::move(volumes.error()));
        }
        std::set<std::string> roots{home_.native()};
        std::set<std::pair<std::uint64_t, std::uint64_t>> identities;
        struct stat home_st {};
        if (::stat(home_.c_str(), &home_st) == 0) {
            identities.emplace(home_st.st_dev, home_st.st_ino);
        }
        for (const VolumeInfo& v : *volumes) {
            for (TrashDir& d : ti::existing_topdir_trashes(ti::normal_path(v.mount_point), uid_)) {
                struct stat st {};
                if (::stat(d.root.c_str(), &st) != 0) {
                    continue;
                }
                if (!roots.insert(d.root.native()).second ||
                    !identities.emplace(st.st_dev, st.st_ino).second) {
                    continue;
                }
                dirs.push_back(std::move(d));
            }
        }
        return dirs;
    }

    [[nodiscard]] Result<TrashListing> list_impl(const std::stop_token& stop) const {
        auto dirs = trash_dirs_impl();
        if (!dirs) {
            return Unexpected<Error>(std::move(dirs.error()));
        }
        TrashListing listing;
        for (const TrashDir& dir : *dirs) {
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "trash list: cancelled");
            }
            const Path info_dir = dir.info_dir();
            Fd info(::open(info_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!info.valid()) {
                if (errno != ENOENT) {
                    listing.dir_errors.push_back(ops_errno_error(errno, "trash: open", info_dir));
                }
                continue;
            }
            const Path files_dir = dir.files_dir();
            Fd files(::open(files_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!files.valid() && errno != ENOENT) {
                listing.dir_errors.push_back(ops_errno_error(errno, "trash: open", files_dir));
                continue;
            }
            auto info_names = read_names(info.get(), info_dir);
            if (!info_names) {
                listing.dir_errors.push_back(std::move(info_names.error()));
                continue;
            }
            std::set<std::string> with_info;
            std::vector<TrashedItem> items;
            for (const std::string& entry : *info_names) {
                if (stop.stop_requested()) {
                    return fail(ErrorCode::Cancelled, "trash list: cancelled");
                }
                if (!is_trashinfo_name(entry)) {
                    continue;
                }
                std::string name = entry.substr(0, entry.size() - 10);
                with_info.insert(name);
                auto text = read_small_file(info.get(), entry.c_str());
                if (!text) {
                    ++listing.orphans;
                    continue;
                }
                auto parsed = parse_trashinfo(*text);
                if (!parsed) {
                    ++listing.orphans;
                    continue;
                }
                auto original = original_path_from(parsed->path, dir.topdir);
                if (!original) {
                    ++listing.orphans;
                    continue;
                }
                struct stat st {};
                if (!files.valid() ||
                    ::fstatat(files.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
                    ++listing.orphans;
                    continue;
                }
                TrashedItem item;
                item.dir = dir;
                item.name = std::move(name);
                item.original_path = std::move(*original);
                item.deletion_date = parsed->deletion_date;
                fill_item_metadata(item, st);
                items.push_back(std::move(item));
            }
            if (files.valid()) {
                auto file_names = read_names(files.get(), files_dir);
                if (!file_names) {
                    listing.dir_errors.push_back(std::move(file_names.error()));
                } else {
                    for (const std::string& name : *file_names) {
                        if (with_info.count(name) == 0) {
                            ++listing.orphans;
                        }
                    }
                }
            }
            std::stable_sort(items.begin(), items.end(),
                             [](const TrashedItem& a, const TrashedItem& b) {
                                 if (a.deletion_date != b.deletion_date) {
                                     if (!a.deletion_date || !b.deletion_date) {
                                         return a.deletion_date.has_value(); // dated first
                                     }
                                     return *a.deletion_date > *b.deletion_date;
                                 }
                                 return a.name < b.name;
                             });
            for (TrashedItem& item : items) {
                listing.items.push_back(std::move(item));
            }
        }
        return listing;
    }

    [[nodiscard]] static Result<void> restore_impl(const TrashedItem& item,
                                                   const Path& destination) {
        if (destination.empty() || !destination.is_absolute()) {
            return fail(ErrorCode::InvalidArgument, "trash restore: relative destination");
        }
        const Path dest = ti::normal_path(destination);
        if (!dest.has_relative_path()) {
            return fail(ErrorCode::InvalidArgument, "trash restore: destination is '/'");
        }
        const Path files = item.files_path();
        struct stat st {};
        if (::lstat(files.c_str(), &st) != 0) {
            return Unexpected<Error>(lookup_error(errno, "trash restore: lstat", files));
        }
        const FileId now_id{static_cast<std::uint64_t>(st.st_dev),
                            static_cast<std::uint64_t>(st.st_ino)};
        if (item.id == FileId{} || now_id != item.id) {
            return fail(ErrorCode::InvalidArgument,
                        "trash restore: '" + files.native() + "' changed since it was listed");
        }
        const Path parent = dest.parent_path();
        struct stat parent_st {};
        if (::stat(parent.c_str(), &parent_st) != 0) {
            return Unexpected<Error>(lookup_error(errno, "trash restore: stat", parent));
        }
        if (!S_ISDIR(parent_st.st_mode)) {
            return fail(ErrorCode::NotFound,
                        "trash restore: '" + parent.native() + "' is not a directory");
        }
        if (rename_noreplace(AT_FDCWD, files.c_str(), AT_FDCWD, dest.c_str()) != 0) {
            const int err = errno;
            Error e = ops_errno_error(err, "trash restore: rename", dest);
            if (err == ENOTEMPTY) {
                e.code = ErrorCode::AlreadyExists;
            }
            return Unexpected<Error>(std::move(e));
        }
        // The item is back; a failure to remove the info file only leaves an orphan, which
        // list() skips.
        (void)::unlink(item.info_path().c_str());
        return ok();
    }

    Path home_;
    std::uint32_t uid_;
    std::string uid_text_;
    VolumeQuery volumes_;
    bool topdirs_;
    std::function<LocalDateTime()> now_;
};

} // namespace

// =============================================================================================
// Topdir trash selection (T4)
// =============================================================================================

} // namespace navigator::trash

namespace navigator::trash_internal {

namespace {

// $topdir/.Trash passes spec check (1): a real directory (not a symlink) with the sticky bit.
[[nodiscard]] bool shared_trash_ok(const Path& shared) noexcept {
    struct stat st {};
    return ::lstat(shared.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX) != 0;
}

[[nodiscard]] TrashDir make_dir(TrashDirKind kind, const Path& root, const Path& topdir) {
    std::uint64_t dev = trash::device_of(root);
    if (dev == 0) {
        dev = trash::device_of(topdir);
    }
    return TrashDir{kind, root, topdir, dev};
}

} // namespace

Result<TrashDir> select_topdir_trash(const Path& topdir_in, std::uint32_t uid, bool create) {
    const Path topdir = normal_path(topdir_in);
    // (1) $topdir/.Trash/$uid
    const Path shared = topdir / ".Trash";
    if (shared_trash_ok(shared)) {
        const Path mine = trash::shared_topdir_trash(topdir, uid);
        struct stat st {};
        bool exists = ::lstat(mine.c_str(), &st) == 0;
        if (!exists && errno == ENOENT) {
            if (!create) {
                if (trash::writable_dir(shared)) {
                    return make_dir(TrashDirKind::SharedUser, mine, topdir);
                }
            } else if (::mkdir(mine.c_str(), 0700) == 0 || errno == EEXIST) {
                exists = ::lstat(mine.c_str(), &st) == 0;
            }
        }
        if (exists && trash::valid_owned_dir(st, uid) && trash::writable_dir(mine)) {
            return make_dir(TrashDirKind::SharedUser, mine, topdir);
        }
        // Otherwise (symlink, not a directory, foreign owner, not creatable): method (2).
    }
    // (2) $topdir/.Trash-$uid
    const Path user = trash::user_topdir_trash(topdir, uid);
    struct stat st {};
    if (::lstat(user.c_str(), &st) != 0) {
        if (errno != ENOENT) {
            return Unexpected<Error>(platform_internal::ops_errno_error(errno, "trash: lstat", user,
                                                                        ErrorCode::Unsupported));
        }
        if (!create) {
            if (trash::writable_dir(topdir)) {
                return make_dir(TrashDirKind::User, user, topdir);
            }
            const int err = errno;
            if (err == EROFS) {
                return Unexpected<Error>(
                    platform_internal::ops_errno_error(err, "trash: topdir", topdir));
            }
            return fail(ErrorCode::Unsupported, "trash: no usable trash on '" + topdir.native() +
                                                    "' (cannot create " + user.filename().native() +
                                                    ")");
        }
        if (::mkdir(user.c_str(), 0700) != 0 && errno != EEXIST) {
            const int err = errno;
            if (err == EROFS) {
                return Unexpected<Error>(
                    platform_internal::ops_errno_error(err, "trash: mkdir", user));
            }
            return fail(ErrorCode::Unsupported, "trash: no usable trash on '" + topdir.native() +
                                                    "': mkdir " + user.native() + ": " +
                                                    platform_internal::errno_text(err));
        }
        if (::lstat(user.c_str(), &st) != 0) {
            return Unexpected<Error>(
                platform_internal::ops_errno_error(errno, "trash: lstat", user));
        }
    }
    if (!trash::valid_owned_dir(st, uid)) {
        return fail(ErrorCode::Unsupported,
                    "trash: '" + user.native() +
                        "' is a symlink, not a directory or not owned by uid " +
                        std::to_string(uid));
    }
    if (!trash::writable_dir(user)) {
        return Unexpected<Error>(
            platform_internal::ops_errno_error(errno, "trash: not writable", user));
    }
    return make_dir(TrashDirKind::User, user, topdir);
}

std::vector<TrashDir> existing_topdir_trashes(const Path& topdir_in, std::uint32_t uid) {
    const Path topdir = normal_path(topdir_in);
    std::vector<TrashDir> out;
    if (shared_trash_ok(topdir / ".Trash")) {
        const Path mine = trash::shared_topdir_trash(topdir, uid);
        struct stat st {};
        if (::lstat(mine.c_str(), &st) == 0 && trash::valid_owned_dir(st, uid)) {
            out.push_back(make_dir(TrashDirKind::SharedUser, mine, topdir));
        }
    }
    const Path user = trash::user_topdir_trash(topdir, uid);
    struct stat st {};
    if (::lstat(user.c_str(), &st) == 0 && trash::valid_owned_dir(st, uid)) {
        out.push_back(make_dir(TrashDirKind::User, user, topdir));
    }
    return out;
}

} // namespace navigator::trash_internal

namespace navigator::trash {

Result<std::shared_ptr<ITrash>> create_trash(TrashOptions options) {
    try {
        Path home = options.home_trash;
        if (home.empty()) {
            auto dirs = platform::data_dirs();
            if (!dirs || dirs->empty()) {
                return fail(ErrorCode::NotFound,
                            "create_trash: no XDG data directory for the home trash");
            }
            home = dirs->front() / "Trash";
        }
        if (!home.is_absolute()) {
            return fail(ErrorCode::InvalidArgument, "create_trash: relative home_trash");
        }
        const std::uint32_t uid = options.uid.value_or(static_cast<std::uint32_t>(::getuid()));
        return std::shared_ptr<ITrash>(
            std::make_shared<Trash>(ti::normal_path(home), uid, std::move(options.volumes),
                                    options.use_topdir_trashes, std::move(options.now)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "create_trash: allocation failed");
    }
}

} // namespace navigator::trash
