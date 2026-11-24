// src/ops/sys.hpp: thin POSIX layer of the ops engine (internal to navigator_ops and tests/ops).
//
// RAII file descriptors, errno -> ErrorCode mapping, stat -> value conversions, renameat2
// wrappers, temp names (S6) and the two TEST SEAMS (I/O fault hook, free-space override). The
// seams are process-global, empty in production, and only tests/ops sets them; they exist because
// ENOSPC / EIO mid-copy and a full destination cannot be produced without root.
//
// Thread-safety: everything is reentrant; the seams are guarded by a mutex (tests install them
// before starting operations and clear them afterwards).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include <navigator/ops.hpp>
#include <navigator/types.hpp>

namespace navigator::ops::detail {

class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept;
    // close() with its result (EIO on close of a written file is a write error). 0 or errno.
    int close_checked() noexcept;

private:
    int fd_ = -1;
};

// errno -> ErrorCode (types.hpp Phase 7 codes included).
[[nodiscard]] ErrorCode code_from_errno(int err) noexcept;
// "<what> '<path>': <strerror>" with the mapped code.
[[nodiscard]] Error errno_error(int err, std::string_view what, const Path& path);
[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

[[nodiscard]] fs::EntryKind kind_of(mode_t mode) noexcept;
[[nodiscard]] Timestamp to_timestamp(const struct timespec& ts) noexcept;
[[nodiscard]] FileId id_of(const struct stat& st) noexcept;
[[nodiscard]] ItemGuard guard_from(const Path& path, const struct stat& st);
[[nodiscard]] ConflictSide side_from(const Path& path, const struct stat& st);
// kind, FileId, size (non-directories) and mtime equal (S12).
[[nodiscard]] bool matches_guard(const struct stat& st, const ItemGuard& guard) noexcept;
[[nodiscard]] bool same_inode(const struct stat& a, const struct stat& b) noexcept;
[[nodiscard]] bool same_inode(const struct stat& st, const FileId& id) noexcept;

// fstatat(dirfd, name, AT_SYMLINK_NOFOLLOW): 0 or errno.
[[nodiscard]] int lstat_at(int dirfd, const std::string& name, struct stat& st) noexcept;
// lstat(path): 0 or errno.
[[nodiscard]] int lstat_path(const Path& path, struct stat& st) noexcept;
// Opens a directory by path (following links in the path: the user addressed it). -1 + errno.
[[nodiscard]] int open_dir_path(const Path& path) noexcept;
// openat(dirfd, name, O_DIRECTORY | O_NOFOLLOW): -1 + errno.
[[nodiscard]] int open_dir_at(int dirfd, const std::string& name) noexcept;

// renameat2(..., RENAME_NOREPLACE): 0 or errno (EINVAL/ENOSYS: flag unsupported).
[[nodiscard]] int rename_noreplace(int olddir, const std::string& oldname, int newdir,
                                   const std::string& newname) noexcept;
// Atomic "rename without replacing" with the S6 fallbacks: RENAME_NOREPLACE, else (regular files
// and links only, `allow_link`) link + unlink(old), else check-then-rename (documented race on
// vfat/exfat). 0 or errno (EEXIST: target taken).
[[nodiscard]] int rename_no_overwrite(int olddir, const std::string& oldname, int newdir,
                                      const std::string& newname, bool allow_link) noexcept;

// 12 lowercase hex characters from getrandom(2).
[[nodiscard]] std::string random_token();
// ".<name>.navigator-<random>.part", <name> cut at a UTF-8 boundary so it fits kNameMaxBytes.
[[nodiscard]] std::string temp_name_for(std::string_view final_name);
// Cuts `text` to at most `max_bytes` without splitting a UTF-8 sequence.
[[nodiscard]] std::string_view utf8_prefix(std::string_view text, std::size_t max_bytes) noexcept;

// The process umask, read from /proc/self/status (umask(2) cannot be read without changing it).
[[nodiscard]] mode_t process_umask() noexcept;

// realpath(parent) / name: the canonical location of an entry WITHOUT following the entry itself.
[[nodiscard]] Result<Path> canonical_entry(const Path& path);

// All names of a directory (no "." / ".."), read through a dup of `dirfd`. 0 or errno.
[[nodiscard]] int read_dir_names(int dirfd, std::vector<std::string>& names);

// Absolute and lexically normal: no ".", "..", empty components or trailing separator.
[[nodiscard]] bool is_normal_absolute(const Path& path);

// statvfs f_bavail * f_frsize of the filesystem holding `dir` (or the test override).
[[nodiscard]] std::optional<std::uint64_t> available_bytes(const Path& dir);

// ---------------------------------------------------------------------------------------------
// Test seams (tests/ops only; empty in production)
// ---------------------------------------------------------------------------------------------

enum class IoSite : std::uint8_t {
    CreateTemp = 0, // before a temp file / link / fifo is created (path: the final destination)
    Write = 1,      // before each data chunk is written (offset: destination offset)
    Fsync = 2,      // before fsync of a copied file
    Commit = 3,     // before the temp name is renamed to the final name
    Rename = 4,     // before a same-filesystem move rename (return EXDEV to force cross-fs)
    Mkdir = 5,      // before a destination directory is created
    Unlink = 6,     // before a source entry of a cross-fs move is removed, or a delete unlinks
};

// Returns 0 to proceed or an errno to inject at that site.
using IoFaultHook = std::function<int(IoSite site, const Path& path, std::uint64_t offset)>;
void set_io_fault_hook(IoFaultHook hook);
[[nodiscard]] int io_fault(IoSite site, const Path& path, std::uint64_t offset = 0);

// Overrides available_bytes() (nullopt from the hook: use statvfs).
using FreeSpaceHook = std::function<std::optional<std::uint64_t>(const Path& dir)>;
void set_free_space_hook(FreeSpaceHook hook);

} // namespace navigator::ops::detail
