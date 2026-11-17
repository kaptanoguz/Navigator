// Shared document file rules (src/core/document_files.hpp). Writes are atomic: unique temporary
// file in the target directory (mode 0600) -> write -> fsync -> rename over the target -> fsync
// the directory.
#include "document_files.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <navigator/layout.hpp>

namespace navigator::core {
namespace {

constexpr std::size_t kTempStemBytes = 200; // keeps ".<stem>.tmp-XXXXXX" within NAME_MAX

ErrorCode map_read_errno(int err) noexcept {
    switch (err) {
    case ENOENT:
    case ENOTDIR: return ErrorCode::NotFound;
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case ENOMEM: return ErrorCode::OutOfMemory;
    default: return ErrorCode::IoError;
    }
}

Error read_error(std::string_view what, const Path& path, int err) {
    return Error{map_read_errno(err), errno_message(what, path, err)};
}

Error write_error(std::string_view what, const Path& path, int err) {
    return Error{map_write_errno(err), errno_message(what, path, err)};
}

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&&) = delete;
    FileDescriptor& operator=(FileDescriptor&&) = delete;
    ~FileDescriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    // Closes now and reports close()'s result (errno preserved on failure).
    int close() noexcept {
        const int fd = std::exchange(fd_, -1);
        return fd >= 0 ? ::close(fd) : 0;
    }

private:
    int fd_;
};

int open_retry(const char* path, int flags, mode_t mode = 0) {
    int fd = -1;
    do {
        fd = ::open(path, flags, mode); // NOLINT(cppcoreguidelines-pro-type-vararg)
    } while (fd < 0 && errno == EINTR);
    return fd;
}

// Creates the missing directories of `dir` with mode 0700 (umask does not apply).
Result<void> make_private_directories(const Path& dir) {
    Path prefix;
    for (const Path& part : dir) {
        prefix /= part;
        struct stat st {};
        if (::stat(prefix.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                return Unexpected<Error>(write_error("not a directory", prefix, ENOTDIR));
            }
            continue;
        }
        if (errno != ENOENT) {
            return Unexpected<Error>(write_error("cannot stat", prefix, errno));
        }
        if (::mkdir(prefix.c_str(), 0700) != 0) {
            if (errno == EEXIST) {
                continue; // created concurrently
            }
            return Unexpected<Error>(write_error("cannot create directory", prefix, errno));
        }
        ::chmod(prefix.c_str(), 0700);
    }
    return ok();
}

} // namespace

std::string errno_message(std::string_view what, const Path& path, int err) {
    std::string msg;
    msg.append(what).append(" '").append(path.native()).append("': ");
    msg.append(std::error_code(err, std::generic_category()).message());
    msg.append(" (errno ").append(std::to_string(err)).append(")");
    return msg;
}

ErrorCode map_write_errno(int err) noexcept {
    switch (err) {
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case ENOMEM: return ErrorCode::OutOfMemory;
    default: return ErrorCode::IoError; // includes ENOSPC, EROFS, EDQUOT, ENOTDIR
    }
}

Result<std::string> read_document(const Path& file) {
    if (file.empty()) {
        return fail(ErrorCode::InvalidArgument, "empty path");
    }
    // O_NONBLOCK: never hang on a FIFO planted at the path; irrelevant for regular files.
    FileDescriptor fd(open_retry(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK));
    if (fd.get() < 0) {
        return Unexpected<Error>(read_error("cannot open", file, errno));
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return Unexpected<Error>(read_error("cannot stat", file, errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return fail(ErrorCode::IoError, "not a regular file: '" + file.native() + "'");
    }
    if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > layout::kMaxDocumentBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "file larger than layout::kMaxDocumentBytes: '" + file.native() + "'");
    }
    std::string text;
    text.resize(static_cast<std::size_t>(st.st_size) + 1);
    std::size_t used = 0;
    for (;;) {
        if (used == text.size()) {
            if (text.size() > layout::kMaxDocumentBytes) {
                return fail(ErrorCode::InvalidArgument,
                            "file larger than layout::kMaxDocumentBytes: '" + file.native() + "'");
            }
            text.resize(std::min(text.size() * 2 + 4096, layout::kMaxDocumentBytes + 1));
        }
        const ssize_t n = ::read(fd.get(), text.data() + used, text.size() - used);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Unexpected<Error>(read_error("cannot read", file, errno));
        }
        if (n == 0) {
            break;
        }
        used += static_cast<std::size_t>(n);
    }
    if (used > layout::kMaxDocumentBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "file larger than layout::kMaxDocumentBytes: '" + file.native() + "'");
    }
    text.resize(used);
    return text;
}

Result<void> write_atomically(const Path& file, std::string_view text) {
    if (file.empty()) {
        return fail(ErrorCode::InvalidArgument, "empty path");
    }
    const std::string name = file.filename().native();
    if (name.empty() || name == "." || name == "..") {
        return fail(ErrorCode::InvalidArgument, "path has no file name: '" + file.native() + "'");
    }
    const Path dir = file.has_parent_path() ? file.parent_path() : Path(".");
    if (Result<void> made = make_private_directories(dir); !made) {
        return made;
    }

    std::string temp = (dir / ("." + name.substr(0, kTempStemBytes) + ".tmp-XXXXXX")).native();
    FileDescriptor fd(::mkostemp(temp.data(), O_CLOEXEC));
    if (fd.get() < 0) {
        return Unexpected<Error>(write_error("cannot create temporary file in", dir, errno));
    }
    const auto discard = [&](Error error) -> Result<void> {
        fd.close();
        ::unlink(temp.c_str());
        return Unexpected<Error>(std::move(error));
    };
    if (::fchmod(fd.get(), 0600) != 0) {
        return discard(write_error("cannot chmod", Path(temp), errno));
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd.get(), text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return discard(write_error("cannot write", Path(temp), errno));
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd.get()) != 0) {
        return discard(write_error("cannot fsync", Path(temp), errno));
    }
    if (fd.close() != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        return Unexpected<Error>(write_error("cannot close", Path(temp), err));
    }
    if (::rename(temp.c_str(), file.c_str()) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        return Unexpected<Error>(write_error("cannot rename into", file, err));
    }
    // Make the rename durable; best effort (some filesystems refuse fsync on directories).
    FileDescriptor dir_fd(open_retry(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir_fd.get() >= 0) {
        ::fsync(dir_fd.get());
    }
    return ok();
}

Error move_aside_bad(const Path& file, std::string reason) {
    const Path bad(file.native() + ".bad");
    if (::rename(file.c_str(), bad.c_str()) == 0) {
        reason += "; moved aside to '" + bad.native() + "'";
    } else {
        reason += "; " + errno_message("could not move aside to", bad, errno);
    }
    return Error{ErrorCode::InvalidArgument, std::move(reason)};
}

} // namespace navigator::core
