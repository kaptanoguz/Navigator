// src/preview/file_util.cpp: bounded file reads and errno mapping for the decoders.
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.hpp"

namespace navigator::preview::detail {

Error errno_error(int err, std::string_view what, const Path& path) {
    ErrorCode code = ErrorCode::IoError;
    switch (err) {
    case ENOENT:
    case ENOTDIR: code = ErrorCode::NotFound; break;
    case EACCES:
    case EPERM: code = ErrorCode::PermissionDenied; break;
    case ENOMEM: code = ErrorCode::OutOfMemory; break;
    case ELOOP:
    case ENAMETOOLONG: code = ErrorCode::InvalidArgument; break;
    default: break;
    }
    std::string msg(what);
    msg += ": ";
    msg += path.native();
    msg += ": ";
    msg += std::strerror(err);
    return Error{code, std::move(msg)};
}

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.release();
    }
    return *this;
}

Fd::~Fd() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Result<Fd> open_regular(const Path& path, std::uint64_t* size, bool nofollow) {
    int flags = O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK;
    if (nofollow) {
        flags |= O_NOFOLLOW;
    }
    Fd fd(::open(path.c_str(), flags));
    if (!fd.valid()) {
        return Unexpected<Error>(errno_error(errno, "open", path));
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return Unexpected<Error>(errno_error(errno, "fstat", path));
    }
    if (!S_ISREG(st.st_mode)) {
        return fail(ErrorCode::InvalidArgument, "not a regular file: " + path.native());
    }
    const int fl = ::fcntl(fd.get(), F_GETFL);
    if (fl >= 0) {
        (void)::fcntl(fd.get(), F_SETFL, fl & ~O_NONBLOCK);
    }
    if (size != nullptr) {
        *size = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0U;
    }
    return fd;
}

long long pread_full(int fd, void* buf, std::size_t len, std::uint64_t offset) {
    std::size_t done = 0;
    auto* p = static_cast<std::uint8_t*>(buf);
    while (done < len) {
        const ssize_t n = ::pread(fd, p + done, len - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    return static_cast<long long>(done);
}

Result<std::vector<std::uint8_t>> read_prefix(int fd, std::uint64_t max_bytes,
                                              const std::stop_token& stop, const Path& path) {
    std::vector<std::uint8_t> out;
    constexpr std::size_t kChunk = std::size_t{1} << 20U;
    try {
        struct stat st {};
        std::uint64_t hint = max_bytes;
        if (::fstat(fd, &st) == 0 && st.st_size >= 0) {
            hint = std::min<std::uint64_t>(hint, static_cast<std::uint64_t>(st.st_size));
        }
        out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(hint, max_bytes)));
        std::uint64_t offset = 0;
        while (offset < max_bytes) {
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            const std::size_t want =
                static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, max_bytes - offset));
            const std::size_t old = out.size();
            out.resize(old + want);
            const long long n = pread_full(fd, out.data() + old, want, offset);
            if (n < 0) {
                return Unexpected<Error>(errno_error(errno, "read", path));
            }
            out.resize(old + static_cast<std::size_t>(n));
            offset += static_cast<std::uint64_t>(n);
            if (static_cast<std::size_t>(n) < want) {
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "read: allocation failed");
    }
    return out;
}

} // namespace navigator::preview::detail
