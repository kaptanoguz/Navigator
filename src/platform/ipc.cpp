// Single-instance Unix domain socket IPC (Phase 10).
// Protocol: each connection sends one message:
//   [uint8_t action][uint32_t big-endian path_len][path bytes (up to 64 KiB)]
// and closes the write side. The server reads non-blockingly in poll().
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <navigator/platform.hpp>

#include "errno_util.hpp"

namespace navigator::platform {

namespace {

constexpr std::size_t kMaxIpcPathBytes = 65536;

Result<sockaddr_un> make_unix_addr(const Path& socket_path) {
    const std::string& native = socket_path.native();
    if (native.empty() || !socket_path.is_absolute()) {
        return fail(ErrorCode::InvalidArgument, "ipc: socket path must be absolute");
    }
    sockaddr_un addr{};
    if (native.size() >= sizeof(addr.sun_path)) {
        return fail(ErrorCode::InvalidArgument, "ipc: socket path exceeds sun_path limit");
    }
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, native.c_str(), native.size() + 1);
    return addr;
}

bool write_all(int fd, const std::uint8_t* data, std::size_t len) noexcept {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_exact(int fd, std::uint8_t* out, std::size_t len) noexcept {
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = ::recv(fd, out + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

class UnixSingleInstanceServer final : public ISingleInstanceServer {
public:
    UnixSingleInstanceServer(int listen_fd, Path socket_path)
        : listen_fd_(listen_fd), socket_path_(std::move(socket_path)) {}

    ~UnixSingleInstanceServer() override {
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (!socket_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(socket_path_, ec);
        }
    }

    std::vector<IpcMessage> poll() override {
        std::vector<IpcMessage> messages;
        if (listen_fd_ < 0) {
            return messages;
        }
        for (int iter = 0; iter < 32; ++iter) {
            const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break; // EAGAIN / EWOULDBLOCK
            }
            // Brief recv timeout so a misbehaving peer cannot stall the UI thread.
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 50'000; // 50 ms
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            std::uint8_t header[5]{};
            if (read_exact(client, header, sizeof(header))) {
                const std::uint8_t raw_action = header[0];
                const std::uint32_t path_len = (static_cast<std::uint32_t>(header[1]) << 24U) |
                                               (static_cast<std::uint32_t>(header[2]) << 16U) |
                                               (static_cast<std::uint32_t>(header[3]) << 8U) |
                                               static_cast<std::uint32_t>(header[4]);
                if (raw_action <= static_cast<std::uint8_t>(IpcAction::SelectPath) &&
                    path_len <= kMaxIpcPathBytes) {
                    std::string path_bytes(path_len, '\0');
                    bool ok_body = true;
                    if (path_len > 0) {
                        ok_body = read_exact(
                            client, reinterpret_cast<std::uint8_t*>(path_bytes.data()), path_len);
                    }
                    if (ok_body) {
                        IpcMessage msg;
                        msg.action = static_cast<IpcAction>(raw_action);
                        if (!path_bytes.empty()) {
                            msg.path = Path(std::move(path_bytes));
                        }
                        messages.push_back(std::move(msg));
                    }
                }
            }
            ::close(client);
        }
        return messages;
    }

    const Path& socket_path() const noexcept override { return socket_path_; }

private:
    int listen_fd_ = -1;
    Path socket_path_;
};

} // namespace

Result<Path> default_ipc_socket_path() {
    try {
        if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg != nullptr && xdg[0] == '/') {
            return Path(xdg) / "navigator.sock";
        }
        return Path("/tmp") / ("navigator-" + std::to_string(::getuid()) + ".sock");
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "default_ipc_socket_path: out of memory");
    }
}

Result<bool> send_to_running_instance(const Path& socket_path, const IpcMessage& message) {
    try {
        auto addr = make_unix_addr(socket_path);
        if (!addr) {
            return Unexpected<Error>(addr.error());
        }
        const std::string& path_bytes = message.path.native();
        if (path_bytes.size() > kMaxIpcPathBytes) {
            return fail(ErrorCode::InvalidArgument, "ipc: path exceeds max message length");
        }

        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return Unexpected<Error>(
                platform_internal::errno_error(errno, "ipc: socket", socket_path));
        }

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 250'000; // 250 ms
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(fd, reinterpret_cast<const sockaddr*>(&*addr), sizeof(*addr)) != 0) {
            const int err = errno;
            ::close(fd);
            if (err == ENOENT || err == ECONNREFUSED) {
                return false; // no live instance
            }
            return Unexpected<Error>(
                platform_internal::errno_error(err, "ipc: connect", socket_path));
        }

        const auto len = static_cast<std::uint32_t>(path_bytes.size());
        std::vector<std::uint8_t> packet(5 + path_bytes.size());
        packet[0] = static_cast<std::uint8_t>(message.action);
        packet[1] = static_cast<std::uint8_t>((len >> 24U) & 0xFFU);
        packet[2] = static_cast<std::uint8_t>((len >> 16U) & 0xFFU);
        packet[3] = static_cast<std::uint8_t>((len >> 8U) & 0xFFU);
        packet[4] = static_cast<std::uint8_t>(len & 0xFFU);
        if (!path_bytes.empty()) {
            std::memcpy(packet.data() + 5, path_bytes.data(), path_bytes.size());
        }

        const bool sent = write_all(fd, packet.data(), packet.size());
        ::close(fd);
        if (!sent) {
            return fail(ErrorCode::IoError, "ipc: failed to write message");
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "send_to_running_instance: out of memory");
    }
}

Result<std::unique_ptr<ISingleInstanceServer>>
create_single_instance_server(const Path& socket_path) {
    try {
        auto addr = make_unix_addr(socket_path);
        if (!addr) {
            return Unexpected<Error>(addr.error());
        }

        // Ensure parent directory exists.
        if (socket_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(socket_path.parent_path(), ec);
        }

        // Check if an existing socket is live; if stale, remove it.
        const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe >= 0) {
            if (::connect(probe, reinterpret_cast<const sockaddr*>(&*addr), sizeof(*addr)) == 0) {
                ::close(probe);
                return fail(ErrorCode::AlreadyExists,
                            "ipc: another Navigator instance is already listening");
            }
            const int err = errno;
            ::close(probe);
            if (err == ECONNREFUSED) {
                std::error_code ec;
                std::filesystem::remove(socket_path, ec);
            }
        }

        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return Unexpected<Error>(
                platform_internal::errno_error(errno, "ipc: socket", socket_path));
        }

        if (::bind(fd, reinterpret_cast<const sockaddr*>(&*addr), sizeof(*addr)) != 0) {
            const int err = errno;
            ::close(fd);
            if (err == EADDRINUSE) {
                return fail(ErrorCode::AlreadyExists, "ipc: socket address in use");
            }
            return Unexpected<Error>(platform_internal::errno_error(err, "ipc: bind", socket_path));
        }

        if (::listen(fd, 16) != 0) {
            const int err = errno;
            ::close(fd);
            std::error_code ec;
            std::filesystem::remove(socket_path, ec);
            return Unexpected<Error>(
                platform_internal::errno_error(err, "ipc: listen", socket_path));
        }

        return std::make_unique<UnixSingleInstanceServer>(fd, socket_path);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "create_single_instance_server: out of memory");
    }
}

} // namespace navigator::platform
