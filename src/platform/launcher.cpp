// Default IAppLauncher: fork + setsid + fork + execve (double fork), so the program is never a
// child or zombie of Navigator. Everything the children need (resolved path, argv, envp, fd
// limits) is prepared before the first fork; between fork and exec only async-signal-safe calls
// run. A CLOEXEC pipe reports the errno of a failed chdir/fork/exec back to the caller; EOF on
// it means exec succeeded.
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <navigator/platform.hpp>

#include "errno_util.hpp"
#include "launcher_internal.hpp"

extern char** environ; // NOLINT(readability-redundant-declaration): POSIX, not in unistd.h

namespace navigator {

namespace platform_internal {

namespace {

bool is_executable_file(const std::string& candidate) {
    struct stat st {};
    if (::stat(candidate.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return ::faccessat(AT_FDCWD, candidate.c_str(), X_OK, AT_EACCESS) == 0;
}

std::string default_path() {
    const std::size_t size = ::confstr(_CS_PATH, nullptr, 0);
    if (size == 0) {
        return "/bin:/usr/bin";
    }
    std::string value(size, '\0');
    (void)::confstr(_CS_PATH, value.data(), size);
    value.resize(size - 1);
    return value;
}

} // namespace

Result<std::string> resolve_program(std::string_view name, const char* path_env) {
    if (name.empty()) {
        return fail(ErrorCode::InvalidArgument, "launch: empty program name");
    }
    if (name.find('\0') != std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "launch: NUL byte in program name");
    }
    if (name.find('/') != std::string_view::npos) {
        std::string direct(name);
        if (is_executable_file(direct)) {
            return direct;
        }
        return fail(ErrorCode::NotFound, "launch: '" + direct + "' is not an executable file");
    }
    const std::string search = path_env != nullptr ? std::string(path_env) : default_path();
    std::size_t start = 0;
    while (start <= search.size()) {
        std::size_t end = search.find(':', start);
        if (end == std::string::npos) {
            end = search.size();
        }
        std::string candidate = search.substr(start, end - start);
        if (candidate.empty()) {
            candidate = ".";
        }
        candidate += '/';
        candidate += name;
        if (is_executable_file(candidate)) {
            return candidate;
        }
        start = end + 1;
    }
    return fail(ErrorCode::NotFound, "launch: '" + std::string(name) + "' not found in $PATH");
}

} // namespace platform_internal

namespace platform {

namespace {

using platform_internal::errno_text;

// What the grandchild reports through the status pipe before _exit().
enum class Stage : int { Chdir = 1, Fork = 2, Exec = 3, Setsid = 4 };

struct ChildReport {
    int stage = 0;
    int err = 0;
};

// Raises `fd` to >= 3 (keeping O_CLOEXEC) so dup2() onto 0..2 can never alias it.
int lift_fd(int fd) {
    if (fd > 2) {
        return fd;
    }
    const int lifted = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    ::close(fd);
    return lifted;
}

class FdGuard {
public:
    explicit FdGuard(int fd = -1) noexcept : fd_(fd) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&&) = delete;
    FdGuard& operator=(FdGuard&&) = delete;
    ~FdGuard() { reset(); }
    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

// Everything the forked processes touch, prepared up front (no allocation after fork).
struct SpawnPlan {
    std::string program;                 // resolved path
    std::vector<std::string> args;       // argv copy
    std::vector<char*> argv;             // pointers into args, nullptr-terminated
    std::vector<std::string> shell_args; // "/bin/sh", program, argv[1..] for ENOEXEC
    std::vector<char*> shell_argv;
    std::string working_dir;
    unsigned int fd_limit = 0; // highest fd + 1 to close when close_range is unavailable
};

void write_report(int fd, Stage stage, int err) noexcept {
    const ChildReport report{static_cast<int>(stage), err};
    const auto* bytes = reinterpret_cast<const char*>(&report); // NOLINT: POD byte view
    std::size_t done = 0;
    while (done < sizeof report) {
        const ssize_t n = ::write(fd, bytes + done, sizeof report - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return;
        }
        done += static_cast<std::size_t>(n);
    }
}

void close_range_fallback(unsigned int first, unsigned int last_inclusive,
                          unsigned int fd_limit) noexcept {
    for (unsigned int fd = first; fd <= last_inclusive && fd < fd_limit; ++fd) {
        ::close(static_cast<int>(fd));
    }
}

void close_fds_except(int keep, unsigned int fd_limit) noexcept {
    const auto keep_u = static_cast<unsigned int>(keep);
#ifdef SYS_close_range
    bool done = true;
    if (keep_u > 3 && ::syscall(SYS_close_range, 3U, keep_u - 1, 0U) != 0) {
        done = false;
    }
    if (done && ::syscall(SYS_close_range, keep_u + 1, ~0U, 0U) == 0) {
        return;
    }
#endif
    if (keep_u > 3) {
        close_range_fallback(3, keep_u - 1, fd_limit);
    }
    close_range_fallback(keep_u + 1, UINT_MAX, fd_limit);
}

// Grandchild: becomes the program. Async-signal-safe calls only.
[[noreturn]] void exec_program(const SpawnPlan& plan, int devnull, int status_fd) noexcept {
    for (int target = 0; target <= 2; ++target) {
        (void)::dup2(devnull, target);
    }
    close_fds_except(status_fd, plan.fd_limit); // also closes devnull (>= 3)

    if (!plan.working_dir.empty() && ::chdir(plan.working_dir.c_str()) != 0) {
        write_report(status_fd, Stage::Chdir, errno);
        ::_exit(127);
    }

    // Default dispositions for every signal, then an empty mask (it was fully blocked).
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL; // NOLINT(cppcoreguidelines-pro-type-union-access)
    ::sigemptyset(&dfl.sa_mask);
    for (int sig = 1; sig < NSIG; ++sig) {
        if (sig != SIGKILL && sig != SIGSTOP) {
            (void)::sigaction(sig, &dfl, nullptr);
        }
    }
    sigset_t empty;
    ::sigemptyset(&empty);
    (void)::sigprocmask(SIG_SETMASK, &empty, nullptr);

    ::execve(plan.program.c_str(), plan.argv.data(), environ);
    if (errno == ENOEXEC) { // execvp rule: run a script without #! through /bin/sh
        ::execve("/bin/sh", plan.shell_argv.data(), environ);
    }
    write_report(status_fd, Stage::Exec, errno);
    ::_exit(127);
}

// Intermediate child: new session, fork the program, exit at once (the program is re-parented
// to init / the nearest subreaper).
[[noreturn]] void run_intermediate(const SpawnPlan& plan, int devnull, int status_fd) noexcept {
    if (::setsid() < 0) {
        write_report(status_fd, Stage::Setsid, errno);
        ::_exit(127);
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        write_report(status_fd, Stage::Fork, errno);
        ::_exit(127);
    }
    if (pid == 0) {
        exec_program(plan, devnull, status_fd);
    }
    ::_exit(0);
}

unsigned int compute_fd_limit() noexcept {
    rlimit limit{};
    constexpr rlim_t kCap = 1U << 20U;
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY ||
        limit.rlim_cur > kCap) {
        return static_cast<unsigned int>(kCap);
    }
    return static_cast<unsigned int>(limit.rlim_cur);
}

std::string_view stage_name(int stage) noexcept {
    switch (static_cast<Stage>(stage)) {
    case Stage::Chdir: return "chdir";
    case Stage::Fork: return "fork";
    case Stage::Exec: return "exec";
    case Stage::Setsid: return "setsid";
    }
    return "spawn";
}

Result<void> spawn_detached(SpawnPlan& plan) {
    plan.argv.clear();
    for (std::string& arg : plan.args) {
        plan.argv.push_back(arg.data());
    }
    plan.argv.push_back(nullptr);
    plan.shell_args.clear();
    plan.shell_args.emplace_back("/bin/sh");
    plan.shell_args.push_back(plan.program);
    plan.shell_args.insert(plan.shell_args.end(), plan.args.begin() + 1, plan.args.end());
    plan.shell_argv.clear();
    for (std::string& arg : plan.shell_args) {
        plan.shell_argv.push_back(arg.data());
    }
    plan.shell_argv.push_back(nullptr);
    plan.fd_limit = compute_fd_limit();

    FdGuard devnull(lift_fd(::open("/dev/null", O_RDWR | O_CLOEXEC)));
    if (devnull.get() < 0) {
        return fail(ErrorCode::PlatformError, "launch: open /dev/null: " + errno_text(errno));
    }
    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return fail(ErrorCode::PlatformError, "launch: pipe: " + errno_text(errno));
    }
    FdGuard read_end(lift_fd(fds[0]));
    FdGuard write_end(lift_fd(fds[1]));
    if (read_end.get() < 0 || write_end.get() < 0) {
        return fail(ErrorCode::PlatformError, "launch: pipe: " + errno_text(errno));
    }

    // Block every signal across fork so no handler of ours runs in the children before they
    // reset dispositions; restored right after in the parent.
    sigset_t all;
    sigset_t old;
    ::sigfillset(&all);
    (void)::pthread_sigmask(SIG_SETMASK, &all, &old);
    const pid_t pid = ::fork();
    if (pid == 0) {
        run_intermediate(plan, devnull.get(), write_end.get());
    }
    const int fork_errno = errno;
    (void)::pthread_sigmask(SIG_SETMASK, &old, nullptr);
    if (pid < 0) {
        return fail(ErrorCode::PlatformError, "launch: fork: " + errno_text(fork_errno));
    }
    write_end.reset();
    devnull.reset();

    // The intermediate child exits immediately; reap it (ECHILD if SIGCHLD is ignored).
    int wstatus = 0;
    while (::waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
    }

    ChildReport report{};
    std::size_t got = 0;
    auto* bytes = reinterpret_cast<char*>(&report); // NOLINT: POD byte view
    while (got < sizeof report) {
        const ssize_t n = ::read(read_end.get(), bytes + got, sizeof report - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    if (got == 0) {
        return ok(); // EOF: exec succeeded (CLOEXEC closed the last write end)
    }
    if (got < sizeof report) {
        return fail(ErrorCode::PlatformError, "launch: truncated status from child");
    }
    std::string message = "launch '";
    message += plan.program;
    message += "': ";
    message += stage_name(report.stage);
    message += ": ";
    message += errno_text(report.err);
    return fail(ErrorCode::PlatformError, std::move(message));
}

class DefaultLauncher final : public IAppLauncher {
public:
    [[nodiscard]] Result<void> launch(const LaunchRequest& request) override {
        try {
            if (request.argv.empty() || request.argv.front().empty()) {
                return fail(ErrorCode::InvalidArgument, "launch: empty argv");
            }
            for (const std::string& arg : request.argv) {
                if (arg.find('\0') != std::string::npos) {
                    return fail(ErrorCode::InvalidArgument, "launch: NUL byte in argument");
                }
            }
            if (request.working_dir.native().find('\0') != std::string::npos) {
                return fail(ErrorCode::InvalidArgument, "launch: NUL byte in working_dir");
            }
            auto resolved =
                platform_internal::resolve_program(request.argv.front(), std::getenv("PATH"));
            if (!resolved) {
                return Unexpected<Error>(std::move(resolved.error()));
            }
            SpawnPlan plan;
            plan.program = std::move(*resolved);
            plan.args = request.argv;
            plan.working_dir = request.working_dir.native();
            return spawn_detached(plan);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "launch: out of memory");
        }
    }
};

} // namespace

Result<std::unique_ptr<IAppLauncher>> create_default_launcher() {
    auto* launcher = new (std::nothrow) DefaultLauncher();
    if (launcher == nullptr) {
        return fail(ErrorCode::OutOfMemory, "create_default_launcher: out of memory");
    }
    return std::unique_ptr<IAppLauncher>(launcher);
}

} // namespace platform

} // namespace navigator
