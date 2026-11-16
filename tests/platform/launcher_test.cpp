// Default IAppLauncher against real processes: `true` from $PATH, the navigator_launch_probe
// helper (reports fds, session, cwd, env, signal state) and temp scripts. Never starts an opener.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include <navigator/platform.hpp>

#include "launcher_internal.hpp"
#include "test_support.hpp"

namespace {

using navigator::ErrorCode;
using navigator::Path;
using navigator::platform::IAppLauncher;
using navigator::platform::LaunchRequest;
using navigator::platform_test::TempDir;
using navigator::platform_test::write_file;
namespace stdfs = std::filesystem;
using namespace std::chrono_literals;

std::unique_ptr<IAppLauncher> make_launcher() {
    auto created = navigator::platform::create_default_launcher();
    REQUIRE(created.has_value());
    REQUIRE(*created != nullptr);
    return std::move(*created);
}

// True when this process has no child at all (running, or a zombie waiting to be reaped).
bool no_children() {
    int status = 0;
    const pid_t pid = ::waitpid(-1, &status, WNOHANG);
    return pid == -1 && errno == ECHILD;
}

bool wait_for_file(const Path& path, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::error_code ec;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stdfs::exists(path, ec)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return stdfs::exists(path, ec);
}

std::map<std::string, std::string> read_report(const Path& path) {
    std::map<std::string, std::string> values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq != std::string::npos) {
            values[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return values;
}

void make_script(const Path& path, const std::string& body) {
    write_file(path, body);
    stdfs::permissions(path, stdfs::perms::owner_all);
}

class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
        }
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
    EnvGuard(EnvGuard&&) = delete;
    EnvGuard& operator=(EnvGuard&&) = delete;
    ~EnvGuard() {
        if (old_) {
            ::setenv(name_, old_->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    void set(const std::string& value) const { ::setenv(name_, value.c_str(), 1); }
    void unset() const { ::unsetenv(name_); }

private:
    const char* name_;
    std::optional<std::string> old_;
};

} // namespace

TEST_CASE("launcher: true from $PATH starts and leaves no child or zombie", "[launcher]") {
    auto launcher = make_launcher();
    REQUIRE(no_children());
    const auto result = launcher->launch(LaunchRequest{{"true"}, {}});
    REQUIRE(result.has_value());
    CHECK(no_children());
    // The program itself is not our child either: give it time to exit and check again.
    std::this_thread::sleep_for(50ms);
    CHECK(no_children());
}

TEST_CASE("launcher: probe sees only /dev/null stdio, its own session, env and argv",
          "[launcher]") {
    TempDir dir;
    const Path marker = dir / "report.txt";

    // An fd without O_CLOEXEC that a careless spawn would leak, plus a dup of it.
    const Path leak_file = dir / "leak.txt";
    write_file(leak_file, "x");
    const int leaked = ::open(leak_file.c_str(), O_RDONLY); // NOLINT: deliberately no CLOEXEC
    REQUIRE(leaked >= 0);
    const int leaked_dup = ::dup(leaked);
    REQUIRE(leaked_dup >= 0);

    EnvGuard env("NAVIGATOR_PROBE_ENV");
    env.set("inherited-value");

    // Ignored dispositions and blocked signals survive exec unless the launcher resets them.
    struct sigaction ignore {};
    struct sigaction old_term {};
    ignore.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore.sa_mask);
    REQUIRE(::sigaction(SIGTERM, &ignore, &old_term) == 0);
    sigset_t block;
    sigset_t old_mask;
    ::sigemptyset(&block);
    ::sigaddset(&block, SIGUSR1);
    REQUIRE(::pthread_sigmask(SIG_BLOCK, &block, &old_mask) == 0);

    auto launcher = make_launcher();
    const auto result = launcher->launch(
        LaunchRequest{{NAVIGATOR_LAUNCH_PROBE, marker.native(), "a b", "", "$HOME;*"}, dir.path()});

    ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    ::sigaction(SIGTERM, &old_term, nullptr);
    ::close(leaked_dup);
    ::close(leaked);

    REQUIRE(result.has_value());
    REQUIRE(wait_for_file(marker));
    const auto report = read_report(marker);
    CHECK(report.at("fds") == "0,1,2");
    CHECK(report.at("stdio_null") == "1");
    // A new session (and process group) whose leader, the intermediate child, already exited:
    // not Navigator's session, and the program can never acquire a controlling terminal.
    CHECK(report.at("sid") != std::to_string(::getsid(0)));
    CHECK(report.at("pgid") == report.at("sid"));
    CHECK(report.at("ppid") != std::to_string(::getpid()));
    CHECK(Path(report.at("cwd")) == dir.path());
    CHECK(report.at("env") == "inherited-value");
    CHECK(report.at("sigmask_empty") == "1");
    CHECK(report.at("sigterm_default") == "1");
    CHECK(report.at("args") == "a b||$HOME;*");
    CHECK(no_children());
}

TEST_CASE("launcher: empty working_dir inherits the current directory", "[launcher]") {
    TempDir dir;
    const Path marker = dir / "report.txt";
    auto launcher = make_launcher();
    REQUIRE(launcher->launch(LaunchRequest{{NAVIGATOR_LAUNCH_PROBE, marker.native()}, {}}));
    REQUIRE(wait_for_file(marker));
    CHECK(Path(read_report(marker).at("cwd")) == stdfs::current_path());
}

TEST_CASE("launcher: argument errors", "[launcher]") {
    auto launcher = make_launcher();
    CHECK(launcher->launch(LaunchRequest{}).error().code == ErrorCode::InvalidArgument);
    CHECK(launcher->launch(LaunchRequest{{""}, {}}).error().code == ErrorCode::InvalidArgument);
    CHECK(launcher->launch(LaunchRequest{{"", "x"}, {}}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(launcher->launch(LaunchRequest{{"true", std::string("a\0b", 3)}, {}}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(no_children());
}

TEST_CASE("launcher: missing or non-executable programs are NotFound synchronously", "[launcher]") {
    TempDir dir;
    const Path plain = dir / "plain";
    write_file(plain, "#!/bin/sh\n");
    stdfs::permissions(plain, stdfs::perms::owner_read | stdfs::perms::owner_write);
    stdfs::create_directories(dir / "subdir");

    auto launcher = make_launcher();
    const auto code = [&](std::vector<std::string> argv) {
        const auto result = launcher->launch(LaunchRequest{std::move(argv), {}});
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    CHECK(code({"navigator-test-no-such-program-4f2a"}) == ErrorCode::NotFound);
    CHECK(code({(dir / "missing").native()}) == ErrorCode::NotFound);
    CHECK(code({plain.native()}) == ErrorCode::NotFound);
    CHECK(code({(dir / "subdir").native()}) == ErrorCode::NotFound);

    // A non-executable file and a directory in $PATH do not count either.
    EnvGuard path("PATH");
    path.set(dir.path().native());
    CHECK(code({"plain"}) == ErrorCode::NotFound);
    CHECK(code({"subdir"}) == ErrorCode::NotFound);
    CHECK(code({"true"}) == ErrorCode::NotFound); // not in this PATH
    CHECK(no_children());
}

TEST_CASE("launcher: exec and chdir failures in the child are PlatformError", "[launcher]") {
    TempDir dir;
    const Path bad_interp = dir / "bad-interp";
    make_script(bad_interp, "#!/nonexistent/navigator-interp\n");
    auto launcher = make_launcher();

    const auto exec_failed = launcher->launch(LaunchRequest{{bad_interp.native()}, {}});
    REQUIRE_FALSE(exec_failed.has_value());
    CHECK(exec_failed.error().code == ErrorCode::PlatformError);
    CHECK(exec_failed.error().message.find("exec") != std::string::npos);

    const auto chdir_failed = launcher->launch(LaunchRequest{{"true"}, dir / "no-such-dir"});
    REQUIRE_FALSE(chdir_failed.has_value());
    CHECK(chdir_failed.error().code == ErrorCode::PlatformError);
    CHECK(chdir_failed.error().message.find("chdir") != std::string::npos);
    CHECK(no_children());
}

TEST_CASE("launcher: a script without #! runs through /bin/sh (execvp rule)", "[launcher]") {
    TempDir dir;
    const Path script = dir / "no-shebang";
    const Path marker = dir / "ran.txt";
    make_script(script, "echo \"ran:$1\" > \"$2\"\n");
    auto launcher = make_launcher();
    REQUIRE(launcher->launch(LaunchRequest{{script.native(), "yes", marker.native()}, {}}));
    REQUIRE(wait_for_file(marker));
    std::ifstream in(marker);
    std::string line;
    std::getline(in, line);
    CHECK(line == "ran:yes");
}

TEST_CASE("launcher: concurrent launches from several threads", "[launcher]") {
    auto launcher = make_launcher();
    std::atomic<int> succeeded{0};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < 4; ++j) {
                if (launcher->launch(LaunchRequest{{"true"}, {}})) {
                    succeeded.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(succeeded.load() == 32);
    CHECK(no_children());
}

TEST_CASE("launcher: resolve_program follows execvp lookup rules", "[launcher]") {
    using navigator::platform_internal::resolve_program;
    TempDir first;
    TempDir second;
    make_script(first / "tool", "#!/bin/sh\n");
    make_script(second / "tool", "#!/bin/sh\n");
    make_script(second / "only-second", "#!/bin/sh\n");
    write_file(first / "only-second", "not executable");

    const std::string path_env = first.path().native() + ":" + second.path().native();
    auto found = resolve_program("tool", path_env.c_str());
    REQUIRE(found.has_value());
    CHECK(Path(*found) == first / "tool"); // first match wins

    found = resolve_program("only-second", path_env.c_str());
    REQUIRE(found.has_value());
    CHECK(Path(*found) == second / "only-second"); // non-executable earlier entry skipped

    found = resolve_program((second / "tool").native(), "");
    REQUIRE(found.has_value());
    CHECK(Path(*found) == second / "tool"); // a name with '/' ignores PATH

    // $PATH unset: confstr(_CS_PATH) contains /bin or /usr/bin, which have sh.
    found = resolve_program("sh", nullptr);
    REQUIRE(found.has_value());
    CHECK(Path(*found).filename() == "sh");

    // Empty entries mean the current directory.
    found = resolve_program("tool", (":" + second.path().native()).c_str());
    REQUIRE(found.has_value());
    CHECK(Path(*found) == second / "tool"); // cwd has no "tool"

    CHECK(resolve_program("", "/bin").error().code == ErrorCode::InvalidArgument);
    CHECK(resolve_program("tool", "/nonexistent-dir").error().code == ErrorCode::NotFound);
    CHECK(resolve_program("tool", "").error().code == ErrorCode::NotFound);
}
