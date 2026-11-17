// fs::Walker (navigator/walk.hpp): breadth-first, streaming, cycle-safe recursive enumeration.
//
// POSIX only. The root is opened once; every queued directory is opened with openat() relative
// to the root fd (O_NOFOLLOW unless follow_symlinks), fstat'ed and compared with the
// (st_dev, st_ino) recorded when it was queued, so a directory replaced between readdir and
// open is reported as vanished instead of being walked under the wrong name. One directory is
// open at a time; a large directory is read across several next_batch() calls.
//
// metadata == false fast path: names and kinds come from d_type; ids of non-directories are
// (st_dev of the directory being read, d_ino), which is exact except for a file bind-mounted
// over another file. Directories (and DT_UNKNOWN entries, and with follow_symlinks the targets
// of symlinks) are fstatat'ed because the descent rules need their st_dev / st_ino.
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <new>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/walk.hpp>

#include "entry_meta.hpp"

namespace navigator::fs {
namespace {

using Clock = std::chrono::steady_clock;

// The stop token is polled every 256 readdir results (contract: at least every 1024) and
// before each directory is opened.
constexpr std::size_t kStopMask = 255;
// The batch deadline is polled every 32 readdir results and at each directory boundary.
constexpr std::size_t kClockMask = 31;

struct FileIdHash {
    std::size_t operator()(const FileId& id) const noexcept {
        // splitmix-style mix of both halves; inode numbers are dense, devices few.
        std::uint64_t x = id.inode ^ (id.device * 0x9E3779B97F4A7C15ULL);
        x ^= x >> 31U;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 29U;
        return static_cast<std::size_t>(x);
    }
};

struct Pending {
    std::uint32_t index = 0; // directory table index
    std::uint32_t depth = 0; // depth of the directory itself (root 0)
    FileId id;               // expected (st_dev, st_ino)
    std::string rel;         // relative path ("" for the root)
};

class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    // Gives up ownership (after fdopendir() took the descriptor).
    void release() noexcept { fd_ = -1; }

private:
    int fd_ = -1;
};

ErrorCode map_root_errno(int err) noexcept {
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

enum class SkipKind : std::uint8_t { Permission, Vanished, Other };

SkipKind classify_dir_errno(int err) noexcept {
    switch (err) {
    case EACCES:
    case EPERM: return SkipKind::Permission;
    case ENOENT:
    case ENOTDIR:
    case ELOOP: // O_NOFOLLOW hit a symlink: the directory was replaced by a link
        return SkipKind::Vanished;
    default: return SkipKind::Other;
    }
}

ErrorCode skip_code(int err) noexcept {
    switch (err) {
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case ENOENT:
    case ENOTDIR: return ErrorCode::NotFound;
    case ENOMEM: return ErrorCode::OutOfMemory;
    case ENAMETOOLONG:
    case ELOOP: return ErrorCode::InvalidArgument;
    default: return ErrorCode::IoError;
    }
}

} // namespace

struct Walker::Impl {
    WalkOptions options;
    WalkStats stats;
    core::Credentials creds;
    Fd root_fd;
    std::uint64_t root_dev = 0;

    std::deque<Pending> queue;
    std::unordered_set<FileId, FileIdHash> entered;
    std::uint32_t next_index = 1;

    // The directory being read.
    DIR* dir = nullptr;
    Pending current;
    std::uint64_t current_dev = 0;

    bool finished = false;
    std::size_t tick = 0; // readdir results since the walk started (stop / clock polling)

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() { close_dir(); }

    void close_dir() noexcept {
        if (dir != nullptr) {
            ::closedir(dir);
            dir = nullptr;
        }
    }

    void finish() noexcept {
        close_dir();
        queue.clear();
        stats.dirs_pending = 0;
        finished = true;
    }

    void sample(const std::string& rel, ErrorCode code) {
        if (stats.samples.size() < kWalkErrorSamples) {
            stats.samples.push_back(WalkSkip{rel, code});
        }
    }

    // Records a directory that could not be opened or read. Returns an error only when the
    // walk must stop (skip_unreadable == false and a permission failure).
    Result<void> skip_dir(const std::string& rel, int err) {
        switch (classify_dir_errno(err)) {
        case SkipKind::Permission:
            if (!options.skip_unreadable) {
                finish();
                return fail(ErrorCode::PermissionDenied,
                            "walk: cannot read directory '" + rel + "'");
            }
            ++stats.skipped_permission;
            sample(rel, ErrorCode::PermissionDenied);
            break;
        case SkipKind::Vanished: ++stats.skipped_vanished; break;
        case SkipKind::Other:
            ++stats.skipped_errors;
            sample(rel, skip_code(err));
            break;
        }
        return {};
    }

    // Opens the next queued directory into `dir`. Returns false when it was skipped.
    Result<bool> open_next() {
        current = std::move(queue.front());
        queue.pop_front();
        stats.dirs_pending = queue.size();
        const char* path = current.rel.empty() ? "." : current.rel.c_str();
        int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
        if (!options.follow_symlinks && !current.rel.empty()) {
            flags |= O_NOFOLLOW;
        }
        Fd fd(::openat(root_fd.get(), path, flags));
        if (fd.get() < 0) {
            const int err = errno;
            auto r = skip_dir(current.rel, err);
            if (!r) {
                return Unexpected<Error>(std::move(r.error()));
            }
            return false;
        }
        struct stat st {};
        if (::fstat(fd.get(), &st) != 0) {
            const int err = errno;
            auto r = skip_dir(current.rel, err);
            if (!r) {
                return Unexpected<Error>(std::move(r.error()));
            }
            return false;
        }
        const FileId got{static_cast<std::uint64_t>(st.st_dev),
                         static_cast<std::uint64_t>(st.st_ino)};
        if (got != current.id) {
            ++stats.skipped_vanished; // replaced since it was queued
            return false;
        }
        DIR* d = ::fdopendir(fd.get());
        if (d == nullptr) {
            const int err = errno;
            auto r = skip_dir(current.rel, err);
            if (!r) {
                return Unexpected<Error>(std::move(r.error()));
            }
            return false;
        }
        fd.release();
        dir = d;
        current_dev = got.device;
        return true;
    }

    // Fills `e` for the readdir result `d` of the current directory. Returns the (dev, ino) the
    // descent rules use and whether the entry is a directory candidate.
    struct Probe {
        bool dir_like = false;
        FileId id;
    };

    Probe describe(WalkEntry& we, const struct dirent* d) {
        DirEntry& e = we.entry;
        const int dfd = ::dirfd(dir);
        Probe probe;
        if (options.metadata) {
            struct stat lst {};
            if (::fstatat(dfd, d->d_name, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
                e.metadata_error = core::map_stat_errno(errno);
                e.kind = core::kind_from_dtype(d->d_type);
                return probe;
            }
            core::describe_entry(e, dfd, d->d_name, lst, options.follow_symlinks, creds);
            probe.id = e.id;
            probe.dir_like =
                e.kind == EntryKind::Directory || (options.follow_symlinks && is_directory_like(e));
            return probe;
        }
        // Fast path.
        e.kind = core::kind_from_dtype(d->d_type);
        e.id = FileId{current_dev, static_cast<std::uint64_t>(d->d_ino)};
        if (e.kind == EntryKind::Directory || e.kind == EntryKind::Unknown) {
            struct stat lst {};
            if (::fstatat(dfd, d->d_name, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
                e.metadata_error = core::map_stat_errno(errno);
                return probe;
            }
            e.kind = core::kind_from_mode(lst.st_mode);
            e.id = FileId{static_cast<std::uint64_t>(lst.st_dev),
                          static_cast<std::uint64_t>(lst.st_ino)};
            probe.dir_like = e.kind == EntryKind::Directory;
            probe.id = e.id;
        }
        if (e.kind == EntryKind::Symlink && options.follow_symlinks) {
            // target_kind stays Unknown on this path (walk.hpp); the target is stat'ed only to
            // decide descent.
            struct stat tst {};
            if (::fstatat(dfd, d->d_name, &tst, 0) == 0 && S_ISDIR(tst.st_mode)) {
                probe.dir_like = true;
                probe.id = FileId{static_cast<std::uint64_t>(tst.st_dev),
                                  static_cast<std::uint64_t>(tst.st_ino)};
                e.id = probe.id;
            }
        }
        return probe;
    }

    // Applies the descent rules to a returned entry; queues it and announces it in `out`.
    void maybe_descend(WalkEntry& we, const Probe& probe, WalkBatch& out) {
        if (!probe.dir_like || we.depth >= options.max_depth) {
            return;
        }
        // Hidden entries were already dropped unless show_hidden.
        if (!options.cross_filesystems && probe.id.device != root_dev) {
            ++stats.skipped_other_fs;
            return;
        }
        if (!entered.insert(probe.id).second) {
            ++stats.skipped_cycles;
            return;
        }
        Pending p;
        p.index = next_index++;
        p.depth = we.depth;
        p.id = probe.id;
        if (!current.rel.empty()) {
            p.rel.reserve(current.rel.size() + 1 + we.entry.name.size());
            p.rel.append(current.rel).push_back('/');
        }
        p.rel.append(we.entry.name);
        out.new_dirs.push_back(p.rel);
        we.self_dir = p.index;
        queue.push_back(std::move(p));
        stats.dirs_pending = queue.size();
    }

    Result<bool> next(WalkBatch& out, const std::stop_token& stop) {
        out.clear();
        out.first_dir = next_index;
        if (finished) {
            return false;
        }
        const Clock::time_point deadline = Clock::now() + options.batch_latency;
        for (;;) {
            if (out.entries.size() >= options.batch_size) {
                return true;
            }
            if (dir == nullptr) {
                if (!out.entries.empty() && Clock::now() >= deadline) {
                    return true;
                }
                if (queue.empty()) {
                    finish();
                    stats.done = true;
                    return false;
                }
                if (stop.stop_requested()) {
                    finish();
                    return fail(ErrorCode::Cancelled, "walk cancelled");
                }
                auto opened = open_next();
                if (!opened) {
                    return Unexpected<Error>(std::move(opened.error()));
                }
                continue;
            }
            const std::size_t t = tick++;
            if ((t & kStopMask) == 0 && stop.stop_requested()) {
                finish();
                return fail(ErrorCode::Cancelled, "walk cancelled");
            }
            if ((t & kClockMask) == 0 && !out.entries.empty() && Clock::now() >= deadline) {
                return true;
            }
            errno = 0;
            const struct dirent* d = ::readdir(dir);
            if (d == nullptr) {
                const int err = errno;
                close_dir();
                if (err != 0) {
                    auto r = skip_dir(current.rel, err);
                    if (!r) {
                        return Unexpected<Error>(std::move(r.error()));
                    }
                } else {
                    ++stats.dirs_visited;
                }
                continue;
            }
            const std::string_view name(d->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            const bool hidden = is_hidden_name(name);
            if (hidden && !options.show_hidden) {
                ++stats.hidden_skipped;
                continue;
            }
            if (options.max_entries != 0 && stats.entries_returned >= options.max_entries) {
                stats.truncated = true;
                stats.done = true;
                finish();
                return false;
            }
            WalkEntry& we = out.entries.emplace_back();
            we.entry.name.assign(name);
            we.entry.hidden = hidden;
            we.dir = current.index;
            we.depth = current.depth + 1;
            const Probe probe = describe(we, d);
            maybe_descend(we, probe, out);
            ++stats.entries_returned;
        }
    }
};

Walker::Walker(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Walker::Walker(Walker&&) noexcept = default;
Walker& Walker::operator=(Walker&&) noexcept = default;
Walker::~Walker() = default;

Result<Walker> Walker::open(const WalkOptions& options) {
    try {
        const std::string& root = options.root.native();
        if (root.empty() || root.front() != '/') {
            return fail(ErrorCode::InvalidArgument, "walk: root must be an absolute path");
        }
        if (options.batch_size == 0) {
            return fail(ErrorCode::InvalidArgument, "walk: batch_size must be > 0");
        }
        if (options.max_depth == 0) {
            return fail(ErrorCode::InvalidArgument, "walk: max_depth must be > 0");
        }
        auto impl = std::make_unique<Impl>();
        impl->options = options;
        impl->root_fd = Fd(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (impl->root_fd.get() < 0) {
            const int err = errno;
            return fail(map_root_errno(err), "walk: cannot open root '" + root + "'");
        }
        struct stat st {};
        if (::fstat(impl->root_fd.get(), &st) != 0) {
            const int err = errno;
            return fail(map_root_errno(err), "walk: cannot stat root '" + root + "'");
        }
        const FileId id{static_cast<std::uint64_t>(st.st_dev),
                        static_cast<std::uint64_t>(st.st_ino)};
        impl->root_dev = id.device;
        impl->entered.insert(id);
        impl->queue.push_back(Pending{0, 0, id, std::string{}});
        impl->stats.dirs_pending = 1;
        return Walker(std::move(impl));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "walk: out of memory");
    }
}

// The by-value std::stop_token parameter is fixed by walk.hpp.
// NOLINTBEGIN(performance-unnecessary-value-param)
Result<bool> Walker::next_batch(WalkBatch& out, std::stop_token stop) {
    // NOLINTEND(performance-unnecessary-value-param)
    try {
        return impl_->next(out, stop);
    } catch (const std::bad_alloc&) {
        impl_->finish();
        out.clear();
        return fail(ErrorCode::OutOfMemory, "walk: out of memory");
    }
}

const WalkStats& Walker::stats() const noexcept { return impl_->stats; }

const WalkOptions& Walker::options() const noexcept { return impl_->options; }

} // namespace navigator::fs
