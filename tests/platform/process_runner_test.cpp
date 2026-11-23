// Default IProcessRunner against real processes: the navigator_runner_probe helper (exit codes,
// output, fds, process group, rlimits, network, file writes), fake bwrap scripts (argument
// construction and the unsandboxed fallback) and, when it works on this machine, the real
// bubblewrap. Temp fixture directories only.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <navigator/platform.hpp>

#include "process_runner_internal.hpp"
#include "test_support.hpp"

namespace {

using navigator::ErrorCode;
using navigator::Path;
using navigator::platform::IProcessRunner;
using navigator::platform::ProcessRequest;
using navigator::platform::ProcessResult;
using navigator::platform_internal::ProcessRunnerOptions;
using navigator::platform_test::ScopedEnv;
using navigator::platform_test::TempDir;
using navigator::platform_test::write_file;
namespace stdfs = std::filesystem;
using namespace std::chrono_literals;

const std::string kProbe = NAVIGATOR_RUNNER_PROBE;

std::unique_ptr<IProcessRunner> make_runner(ProcessRunnerOptions options = {}) {
    auto created = navigator::platform_internal::create_process_runner(std::move(options));
    REQUIRE(created.has_value());
    REQUIRE(*created != nullptr);
    return std::move(*created);
}

ProcessRequest probe(std::vector<std::string> args) {
    ProcessRequest request;
    request.argv.push_back(kProbe);
    request.argv.insert(request.argv.end(), args.begin(), args.end());
    return request;
}

ProcessResult run_ok(IProcessRunner& runner, const ProcessRequest& request,
                     std::stop_token stop = {}) {
    auto result = runner.run(request, std::move(stop));
    if (!result) {
        FAIL("run failed: " << result.error().message);
    }
    return std::move(*result);
}

bool no_children() {
    int status = 0;
    return ::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD;
}

// Dead: no /proc entry, or a zombie waiting for init (it can run nothing any more).
bool process_gone(pid_t pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (!stat) {
        return true;
    }
    std::string line;
    std::getline(stat, line);
    const auto close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() &&
           (line[close + 2] == 'Z' || line[close + 2] == 'X');
}

bool eventually(const auto& predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

std::optional<std::pair<pid_t, pid_t>> read_pids(const Path& file) {
    std::ifstream in(file);
    int a = 0;
    int b = 0;
    if (in >> a >> b) {
        return std::pair{static_cast<pid_t>(a), static_cast<pid_t>(b)};
    }
    return std::nullopt;
}

// True when some live, non-zombie process has `needle` as one of its arguments.
bool any_process_with_arg(const std::string& needle) {
    std::error_code ec;
    for (const auto& entry : stdfs::directory_iterator("/proc", ec)) {
        const std::string name = entry.path().filename().native();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) {
            continue;
        }
        std::ifstream in(entry.path() / "cmdline", std::ios::binary);
        std::string arg;
        while (std::getline(in, arg, '\0')) {
            if (arg == needle && !process_gone(static_cast<pid_t>(std::stoi(name)))) {
                return true;
            }
        }
    }
    return false;
}

std::string slurp(const Path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void make_script(const Path& path, const std::string& body) {
    write_file(path, "#!/bin/sh\n" + body);
    stdfs::permissions(path, stdfs::perms::owner_all);
}

// A TCP listener on 127.0.0.1 (host network namespace); connect() succeeds through the backlog.
class Listener {
public:
    Listener() {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(fd_ >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        REQUIRE(::listen(fd_, 16) == 0);
        socklen_t len = sizeof addr;
        REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
    }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) = delete;
    Listener& operator=(Listener&&) = delete;
    ~Listener() { ::close(fd_); }
    [[nodiscard]] std::string port() const { return std::to_string(port_); }

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

// The real bwrap works here (installed and user namespaces allowed).
bool real_bwrap_works() {
    auto runner = make_runner();
    ProcessRequest request = probe({"exit", "0"});
    request.sandbox = true;
    request.read_only_paths = {kProbe};
    auto result = runner->run(request, {});
    return result && result->sandboxed && result->succeeded();
}

} // namespace

TEST_CASE("process runner: exit codes and signals", "[process_runner]") {
    auto runner = make_runner();
    for (int code : {0, 1, 7, 255}) {
        const ProcessResult result = run_ok(*runner, probe({"exit", std::to_string(code)}));
        CHECK(result.exit_code == code);
        CHECK(result.term_signal == 0);
        CHECK_FALSE(result.timed_out);
        CHECK_FALSE(result.sandboxed);
        CHECK(result.succeeded() == (code == 0));
    }
    const ProcessResult killed = run_ok(*runner, probe({"signal", std::to_string(SIGTERM)}));
    CHECK(killed.exit_code == -1);
    CHECK(killed.term_signal == SIGTERM);
    CHECK_FALSE(killed.succeeded());
    CHECK(no_children());
}

TEST_CASE("process runner: stdout and stderr are captured separately", "[process_runner]") {
    auto runner = make_runner();
    const ProcessResult result = run_ok(*runner, probe({"out", "1000", "500"}));
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_text == std::string(1000, 'o'));
    CHECK(result.stderr_text == std::string(500, 'e'));
    CHECK_FALSE(result.output_truncated);
}

TEST_CASE("process runner: output beyond the cap is discarded, child never blocks",
          "[process_runner]") {
    auto runner = make_runner();
    ProcessRequest request = probe({"out", "3000000", "2000000"});
    request.max_capture_bytes = 4096;
    request.timeout = 20s;
    const ProcessResult result = run_ok(*runner, request);
    CHECK(result.exit_code == 0);
    CHECK_FALSE(result.timed_out);
    CHECK(result.output_truncated);
    CHECK(result.stdout_text == std::string(4096, 'o'));
    CHECK(result.stderr_text == std::string(4096, 'e'));

    request = probe({"out", "4096", "0"});
    request.max_capture_bytes = 4096;
    const ProcessResult exact = run_ok(*runner, request);
    CHECK(exact.stdout_text.size() == 4096);
    CHECK_FALSE(exact.output_truncated);
}

TEST_CASE("process runner: stdin is /dev/null, own process group, cwd, environment",
          "[process_runner]") {
    auto runner = make_runner();
    CHECK(run_ok(*runner, probe({"stdin"})).stdout_text == "0\n");

    std::istringstream ids(run_ok(*runner, probe({"pgid"})).stdout_text);
    int pid = 0;
    int pgid = 0;
    ids >> pid >> pgid;
    CHECK(pid > 0);
    CHECK(pid == pgid);
    CHECK(pgid != ::getpgrp());

    const TempDir tmp;
    ProcessRequest pwd = probe({"pwd"});
    pwd.working_dir = tmp.path();
    CHECK(run_ok(*runner, pwd).stdout_text == tmp.path().native() + "\n");

    const ScopedEnv env("NAVIGATOR_RUNNER_ENV", std::string("from-parent"));
    CHECK(run_ok(*runner, probe({"env", "NAVIGATOR_RUNNER_ENV"})).stdout_text == "from-parent\n");
}

TEST_CASE("process runner: no fd other than 0, 1, 2 is inherited", "[process_runner]") {
    auto runner = make_runner();
    // Deliberately inheritable (no O_CLOEXEC) descriptors in the parent.
    const int leak1 = ::open("/dev/null", O_RDONLY);
    int pipe_fds[2] = {-1, -1};
    REQUIRE(::pipe(pipe_fds) == 0);
    REQUIRE(leak1 >= 0);
    const ProcessResult result = run_ok(*runner, probe({"fds"}));
    ::close(leak1);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    CHECK(result.stdout_text == "0,1,2\n");
}

TEST_CASE("process runner: arguments are never interpreted by a shell", "[process_runner]") {
    auto runner = make_runner();
    const TempDir tmp;
    const Path marker = tmp / "marker";
    const std::vector<std::string> args = {
        "$(touch " + marker.native() + ")",
        "`touch " + marker.native() + "`",
        "; touch " + marker.native(),
        "&& touch " + marker.native(),
        "| touch " + marker.native(),
        "> " + marker.native(),
        "*",
        "a b\tc",
        "$HOME",
        "'quoted' \"double\"",
        "",
        "-n",
    };
    ProcessRequest request = probe({"argv"});
    request.argv.insert(request.argv.end(), args.begin(), args.end());
    request.working_dir = tmp.path();
    const ProcessResult result = run_ok(*runner, request);
    std::string expected;
    for (const std::string& arg : args) {
        expected += arg + "\n";
    }
    CHECK(result.stdout_text == expected);
    CHECK_FALSE(stdfs::exists(marker));
}

TEST_CASE("process runner: timeout kills the child and its grandchild", "[process_runner]") {
    auto runner = make_runner();
    const TempDir tmp;
    const Path pidfile = tmp / "pids";
    ProcessRequest request = probe({"group", pidfile.native()});
    request.timeout = 700ms;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = run_ok(*runner, request);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(result.timed_out);
    CHECK(result.term_signal == SIGKILL);
    CHECK(result.exit_code == -1);
    CHECK_FALSE(result.succeeded());
    CHECK(result.elapsed >= 700ms);
    CHECK(took < 5s);
    const auto pids = read_pids(pidfile);
    REQUIRE(pids.has_value());
    CHECK(eventually([&] { return process_gone(pids->first); }));
    CHECK(eventually([&] { return process_gone(pids->second); }));
    CHECK(no_children());
}

TEST_CASE("process runner: stop_token cancels and kills the group", "[process_runner]") {
    auto runner = make_runner();
    const TempDir tmp;
    const Path pidfile = tmp / "pids";
    ProcessRequest request = probe({"group", pidfile.native()});
    request.timeout = 60s;
    std::stop_source source;
    std::thread stopper([&] {
        (void)eventually([&] { return stdfs::exists(pidfile); });
        source.request_stop();
    });
    const auto start = std::chrono::steady_clock::now();
    auto result = runner->run(request, source.get_token());
    const auto took = std::chrono::steady_clock::now() - start;
    stopper.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(took < 10s);
    const auto pids = read_pids(pidfile);
    REQUIRE(pids.has_value());
    CHECK(eventually([&] { return process_gone(pids->first); }));
    CHECK(eventually([&] { return process_gone(pids->second); }));
    CHECK(no_children());
}

TEST_CASE("process runner: a stop requested before run starts nothing", "[process_runner]") {
    auto runner = make_runner();
    const TempDir tmp;
    std::stop_source source;
    source.request_stop();
    auto result = runner->run(probe({"writefile", (tmp / "f").native(), "1"}), source.get_token());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK_FALSE(stdfs::exists(tmp / "f"));
}

TEST_CASE("process runner: RLIMIT_AS makes a large allocation fail", "[process_runner]") {
    auto runner = make_runner();
    constexpr std::uint64_t kMiB = std::uint64_t{1024} * 1024U;
    ProcessRequest control = probe({"alloc", std::to_string(256 * kMiB)});
    CHECK(run_ok(*runner, control).exit_code == 0);

    ProcessRequest limited = control;
    limited.address_space_bytes = 128 * kMiB;
    CHECK(run_ok(*runner, limited).exit_code == 3);

    ProcessRequest small = probe({"alloc", std::to_string(16 * kMiB)});
    small.address_space_bytes = 128 * kMiB;
    CHECK(run_ok(*runner, small).exit_code == 0);
}

TEST_CASE("process runner: RLIMIT_FSIZE caps written files", "[process_runner]") {
    auto runner = make_runner();
    const TempDir tmp;
    const Path big = tmp / "big";
    ProcessRequest request = probe({"writefile", big.native(), std::to_string(1024 * 1024)});
    request.max_file_bytes = std::uint64_t{64} * 1024U;
    const ProcessResult result = run_ok(*runner, request);
    CHECK_FALSE(result.succeeded());
    CHECK((result.term_signal == SIGXFSZ || result.exit_code == 4));
    CHECK(stdfs::file_size(big) <= std::uintmax_t{64} * 1024U);
}

TEST_CASE("process runner: RLIMIT_CPU stops a spinning child", "[process_runner]") {
    auto runner = make_runner();
    ProcessRequest request = probe({"spin"});
    request.cpu_time = 1s;
    request.timeout = 20s;
    const ProcessResult result = run_ok(*runner, request);
    CHECK_FALSE(result.timed_out);
    CHECK((result.term_signal == SIGXCPU || result.term_signal == SIGKILL));
}

TEST_CASE("process runner: invalid requests", "[process_runner]") {
    auto runner = make_runner();
    auto expect = [&](const ProcessRequest& request, ErrorCode code) {
        auto result = runner->run(request, {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == code);
    };
    expect(ProcessRequest{}, ErrorCode::InvalidArgument);
    ProcessRequest empty_name;
    empty_name.argv = {""};
    expect(empty_name, ErrorCode::InvalidArgument);
    ProcessRequest nul = probe({std::string("a\0b", 3)});
    expect(nul, ErrorCode::InvalidArgument);
    ProcessRequest zero = probe({"exit", "0"});
    zero.timeout = 0ms;
    expect(zero, ErrorCode::InvalidArgument);
    ProcessRequest relative = probe({"exit", "0"});
    relative.sandbox = true;
    relative.read_only_paths = {"relative/path"};
    expect(relative, ErrorCode::InvalidArgument);
    ProcessRequest missing;
    missing.argv = {"navigator-no-such-program-xyz"};
    expect(missing, ErrorCode::NotFound);
    const TempDir tmp;
    write_file(tmp / "plain", "x");
    ProcessRequest not_exec;
    not_exec.argv = {(tmp / "plain").native()};
    expect(not_exec, ErrorCode::NotFound);
    ProcessRequest bad_cwd = probe({"exit", "0"});
    bad_cwd.working_dir = tmp / "missing-dir";
    expect(bad_cwd, ErrorCode::PlatformError);
    CHECK(no_children());
}

TEST_CASE("process runner: concurrent runs reap only their own child", "[process_runner]") {
    auto runner = make_runner();
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic<int> wrong{0};
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            for (int round = 0; round < 5; ++round) {
                auto result = runner->run(probe({"exit", std::to_string(i + 10)}), {});
                if (!result || result->exit_code != i + 10) {
                    ++wrong;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(wrong.load() == 0);
    CHECK(no_children());
}

TEST_CASE("process runner: sandbox without bwrap runs unsandboxed", "[process_runner]") {
    auto runner = make_runner({ProcessRunnerOptions::Bwrap::Absent, {}});
    ProcessRequest request = probe({"exit", "0"});
    request.sandbox = true;
    const ProcessResult result = run_ok(*runner, request);
    CHECK(result.succeeded());
    CHECK_FALSE(result.sandboxed);
}

TEST_CASE("process runner: bwrap that cannot set up namespaces falls back, probed once",
          "[process_runner]") {
    const TempDir tmp;
    const Path log = tmp / "calls";
    const Path fake = tmp / "bwrap";
    make_script(fake, "echo call >> '" + log.native() +
                          "'\necho 'bwrap: setting up uid map: Permission denied' >&2\nexit 1\n");
    auto runner = make_runner({ProcessRunnerOptions::Bwrap::Fixed, fake});
    for (int i = 0; i < 3; ++i) {
        ProcessRequest request = probe({"out", "3", "0"});
        request.sandbox = true;
        const ProcessResult result = run_ok(*runner, request);
        CHECK(result.succeeded());
        CHECK_FALSE(result.sandboxed);
        CHECK(result.stdout_text == "ooo");
    }
    CHECK(slurp(log) == "call\n"); // one probe, cached; real runs never went through it
}

TEST_CASE("process runner: bwrap arguments", "[process_runner]") {
    const TempDir tmp;
    const Path fake = tmp / "bwrap";
    make_script(fake, "for a in \"$@\"; do printf '%s\\n' \"$a\"; done\n");
    const Path input = tmp / "in.png";
    const Path out = tmp / "out";
    write_file(input, "x");
    stdfs::create_directories(out);
    auto runner = make_runner({ProcessRunnerOptions::Bwrap::Fixed, fake});
    const ScopedEnv lang("LANG", std::string("tr_TR.UTF-8"));
    const ScopedEnv lc("LC_ALL", std::nullopt);
    ProcessRequest request = probe({"argv", "$(x)", "a b"});
    request.sandbox = true;
    request.read_only_paths = {input};
    request.writable_paths = {out};
    request.working_dir = out;
    const ProcessResult result = run_ok(*runner, request);
    CHECK(result.sandboxed);
    std::vector<std::string> lines;
    std::istringstream stream(result.stdout_text);
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
    }
    auto has_seq = [&](std::vector<std::string> seq) {
        return std::search(lines.begin(), lines.end(), seq.begin(), seq.end()) != lines.end();
    };
    REQUIRE(lines.size() >= 4);
    CHECK(std::vector(lines.begin(), lines.begin() + 4) ==
          std::vector<std::string>{"--unshare-all", "--die-with-parent", "--new-session",
                                   "--clearenv"});
    CHECK(has_seq({"--setenv", "LANG", "tr_TR.UTF-8"}));
    CHECK_FALSE(has_seq({"--setenv", "LC_ALL"}));
    CHECK(has_seq({"--ro-bind", "/usr", "/usr"}));
    CHECK(has_seq({"--ro-bind", "/etc", "/etc"}));
    CHECK(has_seq({"--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"}));
    CHECK(has_seq({"--ro-bind", input.native(), input.native()}));
    CHECK(has_seq({"--bind", out.native(), out.native()}));
    CHECK(has_seq({"--chdir", out.native()}));
    CHECK(has_seq({"--", stdfs::path(kProbe).lexically_normal().native(), "argv", "$(x)", "a b"}));
    CHECK(lines.back() == "a b");
    // The program and its arguments come only after "--": nothing can be read as a bwrap option.
    const auto dashdash = std::find(lines.begin(), lines.end(), "--");
    CHECK(std::distance(dashdash, lines.end()) == 5);
    // tmpfs /tmp comes before the binds, so paths under /tmp stay visible.
    const auto tmpfs = std::find(lines.begin(), lines.end(), "--tmpfs");
    const auto bind = std::find(lines.begin(), lines.end(), "--bind");
    CHECK(tmpfs < bind);
    // The root is remounted read-only after every bind, before the program.
    const auto remount = std::find(lines.begin(), lines.end(), "--remount-ro");
    REQUIRE(remount != lines.end());
    CHECK(*(remount + 1) == "/");
    CHECK(remount > bind);
    CHECK(remount < dashdash);
}

TEST_CASE("process runner: real bwrap isolates files, network and environment",
          "[process_runner][bwrap]") {
    if (!real_bwrap_works()) {
        SKIP("bubblewrap not installed or user namespaces unavailable");
    }
    auto runner = make_runner();
    const TempDir tmp;
    const Path out = tmp / "out";
    const Path ro = tmp / "ro";
    const Path other = tmp / "other";
    stdfs::create_directories(out);
    stdfs::create_directories(ro);
    stdfs::create_directories(other);
    auto sandboxed = [&](std::vector<std::string> args) {
        ProcessRequest request = probe(std::move(args));
        request.sandbox = true;
        request.read_only_paths = {kProbe, ro};
        request.writable_paths = {out};
        return run_ok(*runner, request);
    };

    const ProcessResult wrote = sandboxed({"writefile", (out / "ok").native(), "10"});
    CHECK(wrote.sandboxed);
    CHECK(wrote.exit_code == 0);
    CHECK(stdfs::file_size(out / "ok") == 10);

    CHECK(sandboxed({"writefile", (ro / "no").native(), "10"}).exit_code == 4);
    CHECK_FALSE(stdfs::exists(ro / "no"));
    CHECK(sandboxed({"writefile", (other / "no").native(), "10"}).exit_code == 4);
    CHECK_FALSE(stdfs::exists(other / "no"));
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        const Path escape = Path(home) / ".navigator-sandbox-escape-test";
        CHECK(sandboxed({"writefile", escape.native(), "1"}).exit_code == 4);
        CHECK_FALSE(stdfs::exists(escape));
    }
    // Parents of bind targets that bwrap created on its root (e.g. the directory holding the
    // probe binary, which may be under $HOME) and the root itself are read-only. A probe that
    // lives under /tmp sits on the sandbox's private tmpfs instead: writable there, never on
    // the host.
    const Path probe_dir_escape = stdfs::path(kProbe).parent_path() / "escape";
    const int probe_dir_exit = sandboxed({"writefile", probe_dir_escape.native(), "1"}).exit_code;
    if (!probe_dir_escape.native().starts_with("/tmp/")) {
        CHECK(probe_dir_exit == 4);
    }
    CHECK_FALSE(stdfs::exists(probe_dir_escape));
    CHECK(sandboxed({"writefile", "/navigator-escape", "1"}).exit_code == 4);
    // /tmp stays a private writable tmpfs: the write succeeds and never reaches the host.
    const std::string private_tmp = "/tmp/navigator-sandbox-private-" + std::to_string(::getpid());
    CHECK(sandboxed({"writefile", private_tmp, "1"}).exit_code == 0);
    CHECK_FALSE(stdfs::exists(private_tmp));

    const Listener listener;
    CHECK(run_ok(*runner, probe({"connect", listener.port()})).exit_code == 0); // control
    CHECK(sandboxed({"connect", listener.port()}).exit_code == 5);

    const ScopedEnv secret("NAVIGATOR_RUNNER_ENV", std::string("secret"));
    CHECK(sandboxed({"env", "NAVIGATOR_RUNNER_ENV"}).stdout_text == "<unset>\n");
    CHECK(sandboxed({"fds"}).stdout_text == "0,1,2\n");
    CHECK(sandboxed({"stdin"}).stdout_text == "0\n");
    CHECK(no_children());
}

TEST_CASE("process runner: real bwrap timeout kills the sandbox", "[process_runner][bwrap]") {
    if (!real_bwrap_works()) {
        SKIP("bubblewrap not installed or user namespaces unavailable");
    }
    auto runner = make_runner();
    const TempDir tmp;
    ProcessRequest request = probe({"group", (tmp / "pids").native()});
    request.sandbox = true;
    request.read_only_paths = {kProbe};
    request.writable_paths = {tmp.path()};
    request.timeout = 1s;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = run_ok(*runner, request);
    CHECK(result.sandboxed);
    CHECK(result.timed_out);
    CHECK(std::chrono::steady_clock::now() - start < 6s);
    CHECK(no_children());
    // The probe and its grandchild ran in the sandbox's pid namespace (their pids are not host
    // pids); bwrap's death takes the namespace down. No host process may still carry the pid
    // file argument.
    const std::string needle = (tmp / "pids").native();
    CHECK(eventually([&] { return !any_process_with_arg(needle); }));
}
