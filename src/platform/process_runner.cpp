// Default IProcessRunner: fork + execve of a resolved program in its own process group, stdin
// /dev/null, stdout/stderr captured through non-blocking pipes, rlimits set in the child, a
// wall-clock deadline and a stop_token (eventfd) that SIGKILL the whole group. Optionally the
// program runs inside bubblewrap. Everything the child touches is prepared before fork(); between
// fork and exec only async-signal-safe calls run. A CLOEXEC status pipe reports a failed
// chdir/exec (EOF on it means exec succeeded).
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <navigator/platform.hpp>

#include "errno_util.hpp"
#include "process_runner_internal.hpp"
#include "spawn_util.hpp"

extern char** environ; // NOLINT(readability-redundant-declaration): POSIX, not in unistd.h

namespace navigator::platform_internal {

namespace {

using platform::ProcessRequest;
using platform::ProcessResult;
using Clock = std::chrono::steady_clock;

// After the direct child exits, pipes are drained for at most this long (a process that left
// the group, e.g. via setsid, may keep them open).
constexpr std::chrono::milliseconds kDrainGrace{500};
// Poll interval when pidfd_open() is unavailable.
constexpr int kFallbackPollMs = 10;
constexpr std::chrono::milliseconds kProbeTimeout{5000};
constexpr std::size_t kReadChunk = std::size_t{64} * 1024U;

enum class Stage : int { Chdir = 1, Exec = 2 };

struct ChildReport {
    int stage = 0;
    int err = 0;
};

// glibc's C++ prototypes take an enum, musl's an int: use whatever RLIMIT_* is.
using RlimitResource = decltype(RLIMIT_AS);

struct Limit {
    RlimitResource resource{};
    rlimit value{};
};

// Everything the forked child touches, prepared up front (no allocation after fork).
struct ExecPlan {
    std::string program;           // absolute path passed to execve
    std::vector<std::string> args; // argv strings
    std::vector<char*> argv;       // pointers into args, nullptr-terminated
    std::string working_dir;       // chdir in the child; empty: inherit
    std::vector<Limit> limits;
    unsigned int fd_limit = 0;
    pid_t parent = 0;

    void finalize() {
        argv.clear();
        for (std::string& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        fd_limit = compute_fd_limit();
        parent = ::getpid();
    }
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

// The child: becomes the program. Async-signal-safe calls only.
[[noreturn]] void run_child(const ExecPlan& plan, int devnull, int out_w, int err_w,
                            int status_w) noexcept {
    (void)::setpgid(0, 0);
    (void)::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != plan.parent) { // the parent died before prctl took effect
        ::_exit(127);
    }
    (void)::dup2(devnull, 0);
    (void)::dup2(out_w, 1);
    (void)::dup2(err_w, 2);
    close_fds_except(status_w, plan.fd_limit); // also closes devnull and the pipe ends (>= 3)

    if (!plan.working_dir.empty() && ::chdir(plan.working_dir.c_str()) != 0) {
        write_report(status_w, Stage::Chdir, errno);
        ::_exit(127);
    }
    for (const Limit& limit : plan.limits) {
        // Best effort: a refused limit leaves the inherited one.
        (void)::setrlimit(limit.resource, &limit.value);
    }

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
    write_report(status_w, Stage::Exec, errno);
    ::_exit(127);
}

Result<void> set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return fail(ErrorCode::PlatformError, "run: fcntl: " + errno_text(errno));
    }
    return ok();
}

// One pipe read by the parent.
struct Sink {
    FdGuard fd;
    std::string* text = nullptr; // nullptr: status pipe (bytes go to `report`)
    std::size_t cap = 0;
    bool truncated = false;
    std::array<char, sizeof(ChildReport)> report{};
    std::size_t report_len = 0;

    [[nodiscard]] bool open() const noexcept { return fd.get() >= 0; }

    // Reads what is available; closes the fd at EOF or on a hard error.
    void drain(std::vector<char>& buffer) {
        while (open()) {
            const ssize_t n = ::read(fd.get(), buffer.data(), buffer.size());
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            if (n <= 0) {
                fd.reset();
                return;
            }
            const auto got = static_cast<std::size_t>(n);
            if (text == nullptr) {
                const std::size_t take = std::min(got, report.size() - report_len);
                std::memcpy(report.data() + report_len, buffer.data(), take);
                report_len += take;
                continue;
            }
            const std::size_t room = cap > text->size() ? cap - text->size() : 0;
            const std::size_t take = std::min(got, room);
            text->append(buffer.data(), take);
            if (take < got) {
                truncated = true; // the rest is read and discarded
            }
        }
    }
};

// True once `pid` has exited (not reaped: WNOWAIT keeps the zombie, and so its process group id,
// until we have killed the rest of the group). ECHILD (SIGCHLD ignored, auto-reaped) counts.
bool child_exited(pid_t pid) noexcept {
    siginfo_t info{};
    for (;;) {
        if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
            return info.si_pid == pid;
        }
        if (errno == EINTR) {
            continue;
        }
        return errno == ECHILD;
    }
}

// Reaps `pid`; returns the wait status or nullopt when it was reaped elsewhere (ECHILD).
std::optional<int> reap(pid_t pid) noexcept {
    int status = 0;
    for (;;) {
        if (::waitpid(pid, &status, 0) == pid) {
            return status;
        }
        if (errno != EINTR) {
            return std::nullopt;
        }
    }
}

int pidfd_for(pid_t pid) noexcept {
#ifdef SYS_pidfd_open
    const long fd = ::syscall(SYS_pidfd_open, pid, 0U);
    if (fd >= 0) {
        (void)::fcntl(static_cast<int>(fd), F_SETFD, FD_CLOEXEC);
        return static_cast<int>(fd);
    }
#endif
    (void)pid;
    return -1;
}

int ms_until(Clock::time_point now, Clock::time_point until) noexcept {
    if (until <= now) {
        return 0;
    }
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(until - now).count();
    return static_cast<int>(std::min<std::int64_t>(ms, 60'000));
}

struct Outcome {
    ProcessResult result;
    bool cancelled = false;
};

// Forks, execs and waits. Never leaves the child unreaped when it returns.
Result<Outcome> execute(ExecPlan& plan, std::chrono::milliseconds timeout, std::size_t cap,
                        const std::stop_token& stop) {
    plan.finalize();
    Outcome outcome;
    ProcessResult& result = outcome.result;

    FdGuard devnull(lift_fd(::open("/dev/null", O_RDWR | O_CLOEXEC)));
    if (devnull.get() < 0) {
        return fail(ErrorCode::PlatformError, "run: open /dev/null: " + errno_text(errno));
    }
    Sink out;
    Sink err;
    Sink status;
    FdGuard out_w;
    FdGuard err_w;
    FdGuard status_w;
    for (auto [sink, write_end] :
         {std::pair{&out, &out_w}, std::pair{&err, &err_w}, std::pair{&status, &status_w}}) {
        std::array<int, 2> fds{-1, -1};
        if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
            return fail(ErrorCode::PlatformError, "run: pipe: " + errno_text(errno));
        }
        sink->fd.reset(lift_fd(fds[0]));
        write_end->reset(lift_fd(fds[1]));
        if (sink->fd.get() < 0 || write_end->get() < 0) {
            return fail(ErrorCode::PlatformError, "run: pipe: " + errno_text(errno));
        }
        if (auto nb = set_nonblocking(sink->fd.get()); !nb) {
            return Unexpected<Error>(std::move(nb.error()));
        }
    }
    out.text = &result.stdout_text;
    err.text = &result.stderr_text;
    out.cap = cap;
    err.cap = cap;

    FdGuard wake(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (wake.get() < 0) {
        return fail(ErrorCode::PlatformError, "run: eventfd: " + errno_text(errno));
    }
    std::vector<char> buffer(kReadChunk);

    const auto start = Clock::now();
    const auto deadline = start + timeout;

    // Block every signal across fork so no handler of ours runs in the child before it resets
    // dispositions; restored right after in the parent.
    sigset_t all;
    sigset_t old;
    ::sigfillset(&all);
    (void)::pthread_sigmask(SIG_SETMASK, &all, &old);
    const pid_t pid = ::fork();
    if (pid == 0) {
        run_child(plan, devnull.get(), out_w.get(), err_w.get(), status_w.get());
    }
    const int fork_errno = errno;
    (void)::pthread_sigmask(SIG_SETMASK, &old, nullptr);
    if (pid < 0) {
        return fail(ErrorCode::PlatformError, "run: fork: " + errno_text(fork_errno));
    }
    (void)::setpgid(pid, pid); // also done by the child; whichever runs first wins
    out_w.reset();
    err_w.reset();
    status_w.reset();
    devnull.reset();

    FdGuard pidfd(pidfd_for(pid));
    const int wake_fd = wake.get();
    const std::stop_callback on_stop(stop, [wake_fd]() noexcept {
        const std::uint64_t one = 1;
        (void)::write(wake_fd, &one, sizeof one);
    });

    bool exited = false;
    Clock::time_point grace{};
    for (;;) {
        if (!exited && child_exited(pid)) {
            exited = true;
            (void)::kill(-pid, SIGKILL); // leftovers in the group; the zombie keeps the pgid
            grace = Clock::now() + kDrainGrace;
        }
        if (exited && !out.open() && !err.open() && !status.open()) {
            break;
        }
        const auto now = Clock::now();
        if (stop.stop_requested()) {
            outcome.cancelled = true;
            break;
        }
        if (!exited && now >= deadline) {
            result.timed_out = true;
            break;
        }
        if (exited && now >= grace) {
            break;
        }

        std::array<pollfd, 5> fds{};
        std::size_t count = 0;
        std::array<Sink*, 5> owner{};
        for (Sink* sink : {&out, &err, &status}) {
            if (sink->open()) {
                owner[count] = sink;
                fds[count++] = pollfd{sink->fd.get(), POLLIN, 0};
            }
        }
        fds[count++] = pollfd{wake_fd, POLLIN, 0};
        if (!exited && pidfd.get() >= 0) {
            fds[count++] = pollfd{pidfd.get(), POLLIN, 0};
        }
        int wait_ms = ms_until(now, exited ? grace : deadline);
        if (!exited && pidfd.get() < 0) {
            wait_ms = std::min(wait_ms, kFallbackPollMs);
        }
        const int ready = ::poll(fds.data(), count, wait_ms);
        if (ready < 0 && errno != EINTR) {
            break; // cannot wait any more: kill and reap below
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (owner[i] != nullptr && (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                owner[i]->drain(buffer);
            }
        }
    }

    (void)::kill(-pid, SIGKILL); // no-op for a group that is already gone
    const std::optional<int> wstatus = reap(pid);
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    status.drain(buffer); // the report (if any) is in the pipe once the child is gone
    if (wstatus) {
        if (WIFEXITED(*wstatus)) {
            result.exit_code = WEXITSTATUS(*wstatus);
        } else if (WIFSIGNALED(*wstatus)) {
            result.term_signal = WTERMSIG(*wstatus);
        }
    }
    result.output_truncated = out.truncated || err.truncated;

    if (status.report_len == sizeof(ChildReport)) {
        ChildReport report{};
        std::memcpy(&report, status.report.data(), sizeof report);
        const bool missing = report.err == ENOENT || report.err == EACCES ||
                             report.err == ENOTDIR || report.err == ELOOP;
        if (static_cast<Stage>(report.stage) == Stage::Exec) {
            return fail(missing ? ErrorCode::NotFound : ErrorCode::PlatformError,
                        "run: exec '" + plan.program + "': " + errno_text(report.err));
        }
        return fail(ErrorCode::PlatformError,
                    "run: chdir '" + plan.working_dir + "': " + errno_text(report.err));
    }
    return outcome;
}

void add_limit(ExecPlan& plan, RlimitResource resource, std::uint64_t wanted, bool cpu) {
    if (wanted == 0) {
        return;
    }
    rlimit current{};
    if (::getrlimit(resource, &current) != 0) {
        return;
    }
    const rlim_t hard = current.rlim_max;
    auto value = static_cast<rlim_t>(wanted);
    if (hard != RLIM_INFINITY && value > hard) {
        value = hard; // an unprivileged process can only lower its hard limit
    }
    rlimit next{value, value};
    if (cpu) {
        // Soft limit: SIGXCPU; one more second until the hard limit's SIGKILL.
        next.rlim_max = (hard == RLIM_INFINITY || value < hard) ? value + 1 : hard;
    }
    plan.limits.push_back(Limit{resource, next});
}

bool has_nul(std::string_view text) noexcept { return text.find('\0') != std::string_view::npos; }

// bwrap arguments (without argv[0]) running `program args...` inside the sandbox.
std::vector<std::string> bwrap_args(const ProcessRequest& request, const std::string& program,
                                    const std::vector<std::string>& args) {
    std::vector<std::string> out = {"--unshare-all", "--die-with-parent", "--new-session",
                                    "--clearenv"};
    for (const char* name : {"PATH", "HOME", "LANG", "LC_ALL"}) {
        if (const char* value = std::getenv(name); value != nullptr) {
            out.insert(out.end(), {"--setenv", name, value});
        }
    }
    for (const char* dir : {"/usr", "/etc"}) {
        struct stat st {};
        if (::stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            out.insert(out.end(), {"--ro-bind", dir, dir});
        }
    }
    for (const char* dir : {"/lib", "/lib64", "/lib32", "/libx32", "/bin", "/sbin"}) {
        struct stat st {};
        if (::lstat(dir, &st) != 0) {
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            std::array<char, 4096> target{};
            const ssize_t n = ::readlink(dir, target.data(), target.size() - 1);
            if (n > 0) {
                out.insert(
                    out.end(),
                    {"--symlink", std::string(target.data(), static_cast<std::size_t>(n)), dir});
            }
        } else if (S_ISDIR(st.st_mode)) {
            out.insert(out.end(), {"--ro-bind", dir, dir});
        }
    }
    out.insert(out.end(), {"--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"});
    for (const Path& path : request.read_only_paths) {
        out.insert(out.end(), {"--ro-bind", path.native(), path.native()});
    }
    for (const Path& path : request.writable_paths) {
        out.insert(out.end(), {"--bind", path.native(), path.native()});
    }
    if (!request.working_dir.empty()) {
        out.insert(out.end(), {"--chdir", request.working_dir.native()});
    }
    out.emplace_back("--");
    out.push_back(program);
    out.insert(out.end(), args.begin() + 1, args.end());
    return out;
}

Result<std::string> absolute_program(std::string_view name) {
    auto found = platform::find_program(name);
    if (!found) {
        return Unexpected<Error>(std::move(found.error()));
    }
    std::error_code ec;
    Path absolute = std::filesystem::absolute(*found, ec);
    if (ec) {
        return fail(ErrorCode::PlatformError,
                    "run: cannot make '" + found->native() + "' absolute: " + ec.message());
    }
    return absolute.lexically_normal().native();
}

Result<void> validate(const ProcessRequest& request) {
    if (request.argv.empty() || request.argv.front().empty()) {
        return fail(ErrorCode::InvalidArgument, "run: empty argv");
    }
    if (request.timeout <= std::chrono::milliseconds::zero()) {
        return fail(ErrorCode::InvalidArgument, "run: timeout must be > 0");
    }
    for (const std::string& arg : request.argv) {
        if (has_nul(arg)) {
            return fail(ErrorCode::InvalidArgument, "run: NUL byte in argument");
        }
    }
    if (has_nul(request.working_dir.native())) {
        return fail(ErrorCode::InvalidArgument, "run: NUL byte in working_dir");
    }
    if (request.sandbox) {
        for (const auto* list : {&request.read_only_paths, &request.writable_paths}) {
            for (const Path& path : *list) {
                if (path.empty() || !path.is_absolute() || has_nul(path.native())) {
                    return fail(ErrorCode::InvalidArgument,
                                "run: sandbox paths must be absolute: '" + path.native() + "'");
                }
            }
        }
    }
    return ok();
}

class DefaultProcessRunner final : public platform::IProcessRunner {
public:
    explicit DefaultProcessRunner(ProcessRunnerOptions options) : options_(std::move(options)) {}

    [[nodiscard]] Result<ProcessResult> run(const ProcessRequest& request,
                                            std::stop_token stop) override {
        try {
            if (auto valid = validate(request); !valid) {
                return Unexpected<Error>(std::move(valid.error()));
            }
            auto program = absolute_program(request.argv.front());
            if (!program) {
                return Unexpected<Error>(std::move(program.error()));
            }
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "run: cancelled before start");
            }

            ExecPlan plan;
            bool sandboxed = false;
            if (request.sandbox) {
                auto bwrap = usable_bwrap(stop);
                if (!bwrap) {
                    return Unexpected<Error>(std::move(bwrap.error()));
                }
                if (const std::optional<std::string>& path = *bwrap; path.has_value()) {
                    sandboxed = true;
                    plan.program = *path;
                    plan.args = {plan.program};
                    auto inner = bwrap_args(request, *program, request.argv);
                    plan.args.insert(plan.args.end(), std::make_move_iterator(inner.begin()),
                                     std::make_move_iterator(inner.end()));
                }
            }
            if (!sandboxed) {
                plan.program = std::move(*program);
                plan.args = request.argv;
                plan.working_dir = request.working_dir.native();
            }
            add_limit(plan, RLIMIT_AS, request.address_space_bytes, false);
            add_limit(plan, RLIMIT_CPU, static_cast<std::uint64_t>(request.cpu_time.count()), true);
            add_limit(plan, RLIMIT_FSIZE, request.max_file_bytes, false);

            auto outcome = execute(plan, request.timeout, request.max_capture_bytes, stop);
            if (!outcome) {
                return Unexpected<Error>(std::move(outcome.error()));
            }
            if (outcome->cancelled) {
                return fail(ErrorCode::Cancelled, "run: cancelled");
            }
            outcome->result.sandboxed = sandboxed;
            return std::move(outcome->result);
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "run: out of memory");
        }
    }

private:
    // The bwrap binary to use, nullopt when sandboxing is unavailable (absent or probe failed).
    Result<std::optional<std::string>> usable_bwrap(const std::stop_token& stop) {
        std::string bwrap;
        switch (options_.bwrap) {
        case ProcessRunnerOptions::Bwrap::Absent: return std::optional<std::string>{};
        case ProcessRunnerOptions::Bwrap::Fixed: bwrap = options_.bwrap_path.native(); break;
        case ProcessRunnerOptions::Bwrap::Lookup: {
            auto found = platform::find_program("bwrap");
            if (!found) {
                return std::optional<std::string>{};
            }
            bwrap = found->native();
            break;
        }
        }
        const std::lock_guard lock(mutex_);
        if (auto it = probed_.find(bwrap); it != probed_.end()) {
            return it->second ? std::optional<std::string>{bwrap} : std::nullopt;
        }
        auto probe = probe_bwrap(bwrap, stop);
        if (!probe) {
            return Unexpected<Error>(std::move(probe.error()));
        }
        probed_.emplace(bwrap, *probe);
        return *probe ? std::optional<std::string>{bwrap} : std::nullopt;
    }

    // Runs `true` inside bwrap with the namespace flags of a real run. Cancelled propagates;
    // every other failure means "sandbox unavailable".
    static Result<bool> probe_bwrap(const std::string& bwrap, const std::stop_token& stop) {
        auto truth = absolute_program("true");
        if (!truth) {
            return true; // nothing to probe with; let the real run decide
        }
        ExecPlan plan;
        plan.program = bwrap;
        plan.args = {bwrap};
        auto inner = bwrap_args(ProcessRequest{}, *truth, {*truth});
        plan.args.insert(plan.args.end(), inner.begin(), inner.end());
        auto outcome = execute(plan, kProbeTimeout, 4096, stop);
        if (!outcome) {
            return false;
        }
        if (outcome->cancelled) {
            return fail(ErrorCode::Cancelled, "run: cancelled while probing bwrap");
        }
        return outcome->result.succeeded();
    }

    ProcessRunnerOptions options_;
    std::mutex mutex_;
    std::map<std::string, bool> probed_; // bwrap path -> probe succeeded
};

} // namespace

Result<std::unique_ptr<platform::IProcessRunner>>
create_process_runner(ProcessRunnerOptions options) {
    try {
        return std::unique_ptr<platform::IProcessRunner>(
            std::make_unique<DefaultProcessRunner>(std::move(options)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "create_default_process_runner: out of memory");
    }
}

} // namespace navigator::platform_internal

namespace navigator::platform {

Result<std::unique_ptr<IProcessRunner>> create_default_process_runner() {
    return platform_internal::create_process_runner({});
}

} // namespace navigator::platform
