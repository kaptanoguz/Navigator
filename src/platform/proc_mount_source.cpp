// Default IMountSource: mountinfo file + statvfs(). Stateless apart from the change_fd, which is
// only ever polled, so one instance may be shared between threads.
#include <cerrno>
#include <memory>
#include <new>
#include <string>

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <navigator/volume.hpp>

#include "errno_util.hpp"

namespace navigator {

namespace {

using platform_internal::errno_error;

class ProcMountSource final : public IMountSource {
public:
    ProcMountSource(Path path, int fd) noexcept : path_(std::move(path)), fd_(fd) {}
    ProcMountSource(const ProcMountSource&) = delete;
    ProcMountSource& operator=(const ProcMountSource&) = delete;
    ProcMountSource(ProcMountSource&&) = delete;
    ProcMountSource& operator=(ProcMountSource&&) = delete;
    ~ProcMountSource() override { ::close(fd_); }

    Result<std::string> read_mountinfo() override {
        // A fresh descriptor per call: no shared file offset, so concurrent calls are safe.
        const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return Unexpected<Error>(errno_error(errno, "mountinfo: cannot open", path_));
        }
        std::string text;
        try {
            std::string chunk(16384, '\0');
            for (;;) {
                const ssize_t n = ::read(fd, chunk.data(), chunk.size());
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    const int err = errno;
                    ::close(fd);
                    return Unexpected<Error>(errno_error(err, "mountinfo: cannot read", path_));
                }
                if (n == 0) {
                    break;
                }
                text.append(chunk.data(), static_cast<std::size_t>(n));
            }
        } catch (const std::bad_alloc&) {
            ::close(fd);
            return fail(ErrorCode::OutOfMemory, "mountinfo: allocation failed");
        }
        ::close(fd);
        return text;
    }

    Result<SpaceInfo> query_space(const Path& mount_point) override {
        struct statvfs st {};
        if (::statvfs(mount_point.c_str(), &st) != 0) {
            const int err = errno;
            ErrorCode code = ErrorCode::IoError;
            if (err == ENOENT || err == ENOTDIR) {
                code = ErrorCode::NotFound;
            } else if (err == EACCES || err == EPERM) {
                code = ErrorCode::PermissionDenied;
            }
            Error error = errno_error(err, "statvfs", mount_point);
            error.code = code;
            return Unexpected<Error>(std::move(error));
        }
        const auto frsize = static_cast<std::uint64_t>(st.f_frsize);
        SpaceInfo space;
        space.total_bytes = static_cast<std::uint64_t>(st.f_blocks) * frsize;
        space.free_bytes = static_cast<std::uint64_t>(st.f_bfree) * frsize;
        space.available_bytes = static_cast<std::uint64_t>(st.f_bavail) * frsize;
        return space;
    }

    [[nodiscard]] int change_fd() const noexcept override { return fd_; }

private:
    Path path_;
    int fd_;
};

} // namespace

Result<std::shared_ptr<IMountSource>> create_proc_mount_source(const Path& mountinfo_path) {
    const int fd = ::open(mountinfo_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        Error error = errno_error(err, "mountinfo: cannot open", mountinfo_path);
        if (error.code != ErrorCode::NotFound && error.code != ErrorCode::PermissionDenied) {
            error.code = ErrorCode::IoError;
        }
        return Unexpected<Error>(std::move(error));
    }
    try {
        return std::shared_ptr<IMountSource>(std::make_shared<ProcMountSource>(mountinfo_path, fd));
    } catch (const std::bad_alloc&) {
        ::close(fd);
        return fail(ErrorCode::OutOfMemory, "mountinfo: allocation failed");
    }
}

} // namespace navigator
