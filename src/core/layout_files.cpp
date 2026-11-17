// Session and named-layout files (navigator/layout.hpp "Files"). POSIX I/O only; no XDG lookup
// (the caller passes the config root). Writes are atomic: unique temporary file in the target
// directory (mode 0600) -> write -> fsync -> rename over the target -> fsync the directory.
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/layout.hpp>

#include "utf8.hpp"

namespace navigator::layout {
namespace {

constexpr std::string_view kJsonSuffix = ".json";
constexpr std::size_t kMaxLayoutNameBytes = 64;
constexpr std::size_t kTempStemBytes = 200; // keeps ".<stem>.tmp-XXXXXX" within NAME_MAX

std::string errno_message(std::string_view what, const Path& path, int err) {
    std::string msg;
    msg.append(what).append(" '").append(path.native()).append("': ");
    msg.append(std::error_code(err, std::generic_category()).message());
    msg.append(" (errno ").append(std::to_string(err)).append(")");
    return msg;
}

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

ErrorCode map_write_errno(int err) noexcept {
    switch (err) {
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case ENOMEM: return ErrorCode::OutOfMemory;
    default: return ErrorCode::IoError; // includes ENOSPC, EROFS, EDQUOT, ENOTDIR
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

// Reads a whole regular file of at most kMaxDocumentBytes.
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
    if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > kMaxDocumentBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "file larger than kMaxDocumentBytes: '" + file.native() + "'");
    }
    std::string text;
    text.resize(static_cast<std::size_t>(st.st_size) + 1);
    std::size_t used = 0;
    for (;;) {
        if (used == text.size()) {
            if (text.size() > kMaxDocumentBytes) {
                return fail(ErrorCode::InvalidArgument,
                            "file larger than kMaxDocumentBytes: '" + file.native() + "'");
            }
            text.resize(std::min(text.size() * 2 + 4096, kMaxDocumentBytes + 1));
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
    if (used > kMaxDocumentBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "file larger than kMaxDocumentBytes: '" + file.native() + "'");
    }
    text.resize(used);
    return text;
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

Path layout_file(const Path& dir, std::string_view name) {
    std::string file_name(name);
    file_name.append(kJsonSuffix);
    return dir / file_name;
}

Result<void> check_named(const Path& dir, std::string_view name) {
    if (!is_valid_layout_name(name)) {
        return fail(ErrorCode::InvalidArgument, "invalid layout name");
    }
    if (dir.empty()) {
        return fail(ErrorCode::InvalidArgument, "empty layouts directory");
    }
    return ok();
}

} // namespace

Result<Session> load_session(const Path& file) {
    try {
        Result<std::string> text = read_document(file);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        Result<Session> session = session_from_json(*text);
        if (!session && session.error().code == ErrorCode::InvalidArgument) {
            const Path bad(file.native() + ".bad");
            std::string message = session.error().message;
            if (::rename(file.c_str(), bad.c_str()) == 0) {
                message += "; moved aside to '" + bad.native() + "'";
            } else {
                message += "; " + errno_message("could not move aside to", bad, errno);
            }
            return fail(ErrorCode::InvalidArgument, std::move(message));
        }
        return session;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while loading the session");
    }
}

Result<void> save_session(const Path& file, const Session& session) {
    try {
        if (file.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty path");
        }
        Result<std::string> text = to_json(session);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        return write_atomically(file, *text);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while saving the session");
    }
}

bool is_valid_layout_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxLayoutNameBytes) {
        return false;
    }
    if (name.front() == '.' || name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    for (const char c : name) {
        const auto b = static_cast<unsigned char>(c);
        if (b == '/' || b < 0x20 || b == 0x7F) {
            return false;
        }
    }
    return core::is_valid_utf8(name);
}

Result<void> save_named_layout(const Path& dir, std::string_view name, const Layout& layout) {
    try {
        if (Result<void> checked = check_named(dir, name); !checked) {
            return checked;
        }
        Result<std::string> text = layout_to_json(layout);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        return write_atomically(layout_file(dir, name), *text);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while saving a layout");
    }
}

Result<Layout> load_named_layout(const Path& dir, std::string_view name) {
    try {
        if (Result<void> checked = check_named(dir, name); !checked) {
            return Unexpected<Error>(checked.error());
        }
        Result<std::string> text = read_document(layout_file(dir, name));
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        return layout_from_json(*text);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while loading a layout");
    }
}

Result<std::vector<std::string>> list_named_layouts(const Path& dir) {
    try {
        Result<std::vector<fs::DirEntry>> entries = fs::list_directory(dir);
        if (!entries) {
            switch (entries.error().code) {
            case ErrorCode::NotFound: return std::vector<std::string>{};
            case ErrorCode::InvalidArgument:
                return fail(ErrorCode::IoError, entries.error().message);
            default: return Unexpected<Error>(entries.error());
            }
        }
        std::vector<std::string> names;
        for (const fs::DirEntry& entry : *entries) {
            const bool regular =
                entry.kind == fs::EntryKind::File ||
                (entry.kind == fs::EntryKind::Symlink && entry.target_kind == fs::EntryKind::File);
            const std::string_view file_name = entry.name;
            if (!regular || file_name.size() <= kJsonSuffix.size() ||
                !file_name.ends_with(kJsonSuffix)) {
                continue;
            }
            const std::string_view stem =
                file_name.substr(0, file_name.size() - kJsonSuffix.size());
            if (is_valid_layout_name(stem)) {
                names.emplace_back(stem);
            }
        }
        const fs::SortOptions order;
        std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
            const std::weak_ordering c = fs::compare_names(a, b, order);
            return c != 0 ? c < 0 : a < b;
        });
        return names;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while listing layouts");
    }
}

Result<void> delete_named_layout(const Path& dir, std::string_view name) {
    try {
        if (Result<void> checked = check_named(dir, name); !checked) {
            return checked;
        }
        const Path file = layout_file(dir, name);
        if (::unlink(file.c_str()) != 0) {
            const int err = errno;
            return Unexpected<Error>(
                Error{err == ENOENT || err == ENOTDIR ? ErrorCode::NotFound : map_write_errno(err),
                      errno_message("cannot delete", file, err)});
        }
        return ok();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while deleting a layout");
    }
}

} // namespace navigator::layout
