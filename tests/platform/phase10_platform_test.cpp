#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/git.hpp>
#include <navigator/platform.hpp>

#include "test_support.hpp"

namespace {

using navigator::ErrorCode;
using navigator::fail;
using navigator::ok;
using navigator::Path;
using navigator::Result;
using navigator::platform::IAppLauncher;
using navigator::platform::IpcAction;
using navigator::platform::IpcMessage;
using navigator::platform::IProcessRunner;
using navigator::platform::LaunchRequest;
using navigator::platform::ProcessRequest;
using navigator::platform::ProcessResult;

class FakeTerminalLauncher final : public IAppLauncher {
public:
    std::string accept_program;
    std::vector<LaunchRequest> recorded;

    Result<void> launch(const LaunchRequest& request) override {
        recorded.push_back(request);
        if (!request.argv.empty() && request.argv[0] == accept_program) {
            return ok();
        }
        return fail(ErrorCode::NotFound, "not found");
    }
};

class FakeGitRunner final : public IProcessRunner {
public:
    std::string stdout_payload;
    int calls = 0;

    Result<ProcessResult> run(const ProcessRequest& /*request*/,
                              std::stop_token /*stop*/) override {
        ++calls;
        ProcessResult r;
        r.exit_code = 0;
        r.stdout_text = stdout_payload;
        return r;
    }
};

} // namespace

TEST_CASE("platform::open_terminal resolves fallback order and validates dir", "[platform][p10]") {
    navigator::platform_test::TempDir tmp;
    const Path file = tmp.path() / "file.txt";
    {
        std::ofstream out(file);
        out << "hi";
    }

    FakeTerminalLauncher fake;
    CHECK_FALSE(navigator::platform::open_terminal(Path("relative/dir"), fake).has_value());
    CHECK_FALSE(navigator::platform::open_terminal(file, fake).has_value());
    CHECK_FALSE(navigator::platform::open_terminal(tmp.path() / "missing", fake).has_value());

    fake.accept_program = "kitty";
    auto res = navigator::platform::open_terminal(tmp.path(), fake);
    REQUIRE(res.has_value());
    REQUIRE_FALSE(fake.recorded.empty());
    CHECK(fake.recorded.front().argv[0] == "xdg-terminal-exec");
    CHECK(fake.recorded.back().argv[0] == "kitty");
    CHECK(fake.recorded.back().working_dir == tmp.path());

    fake.recorded.clear();
    fake.accept_program = "nonexistent-terminal";
    auto none = navigator::platform::open_terminal(tmp.path(), fake);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == ErrorCode::Unsupported);
}

TEST_CASE("platform::is_gvfs_uri classifies remote and device schemes", "[platform][p10]") {
    CHECK(navigator::platform::is_gvfs_uri("smb://nas.local/share"));
    CHECK(navigator::platform::is_gvfs_uri("MTP://Pixel_8/Internal"));
    CHECK(navigator::platform::is_gvfs_uri("sftp://user@host/home"));
    CHECK(navigator::platform::is_gvfs_uri("davs://cloud.example/remote"));
    CHECK_FALSE(navigator::platform::is_gvfs_uri("/home/user"));
    CHECK_FALSE(navigator::platform::is_gvfs_uri("file:///home/user"));
    CHECK_FALSE(navigator::platform::is_gvfs_uri("relative/path"));

    auto backend = navigator::platform::create_gvfs_backend();
    REQUIRE(backend.has_value());
    auto mounts = (*backend)->list_mounts();
    CHECK(mounts.has_value());
}

TEST_CASE("platform single-instance IPC round-trip and AlreadyExists", "[platform][p10][ipc]") {
    navigator::platform_test::TempDir tmp;
    const Path sock = tmp.path() / "nav.sock";

    // No server yet -> send returns false.
    auto before =
        navigator::platform::send_to_running_instance(sock, IpcMessage{IpcAction::NewTab, "/tmp"});
    REQUIRE(before.has_value());
    CHECK(*before == false);

    auto server = navigator::platform::create_single_instance_server(sock);
    REQUIRE(server.has_value());

    // Second server on same live socket must fail with AlreadyExists.
    auto dup = navigator::platform::create_single_instance_server(sock);
    REQUIRE_FALSE(dup.has_value());
    CHECK(dup.error().code == ErrorCode::AlreadyExists);

    // Send two messages and poll them.
    auto s1 = navigator::platform::send_to_running_instance(
        sock, IpcMessage{IpcAction::NewTab, Path("/var/tmp/dir a")});
    REQUIRE(s1.has_value());
    CHECK(*s1 == true);

    auto s2 = navigator::platform::send_to_running_instance(
        sock, IpcMessage{IpcAction::SelectPath, Path("/var/tmp/file.txt")});
    REQUIRE(s2.has_value());
    CHECK(*s2 == true);

    auto msgs = (*server)->poll();
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].action == IpcAction::NewTab);
    CHECK(msgs[0].path == Path("/var/tmp/dir a"));
    CHECK(msgs[1].action == IpcAction::SelectPath);
    CHECK(msgs[1].path == Path("/var/tmp/file.txt"));

    // Subsequent poll is empty.
    CHECK((*server)->poll().empty());
}

TEST_CASE("git::parse_porcelain_v1_z parses branch, statuses, renames and dir propagation",
          "[platform][p10][git]") {
    std::string raw;
    auto push = [&](std::string_view s) {
        raw.append(s);
        raw.push_back('\0');
    };
    push("## phase-10...origin/phase-10 [ahead 2]");
    push(" M src/app/main.cpp");
    push("A  src/new_file.cpp");
    push("?? untracked.txt");
    push("UU conflict.txt");
    push("R  src/renamed.cpp");
    push("src/old_name.cpp");
    push("!! build/out.o");

    const Path root("/repo");
    const auto snap = navigator::git::parse_porcelain_v1_z(raw, root);
    CHECK(snap.branch == "phase-10");
    CHECK(snap.file_states.at("src/app/main.cpp") == navigator::git::GitFileState::Modified);
    CHECK(snap.file_states.at("src/new_file.cpp") == navigator::git::GitFileState::Added);
    CHECK(snap.file_states.at("untracked.txt") == navigator::git::GitFileState::Untracked);
    CHECK(snap.file_states.at("conflict.txt") == navigator::git::GitFileState::Conflict);
    CHECK(snap.file_states.at("src/renamed.cpp") == navigator::git::GitFileState::Renamed);
    CHECK(snap.file_states.at("build/out.o") == navigator::git::GitFileState::Ignored);

    // Directory propagation: src/app -> Modified, src -> Renamed (higher severity than
    // Modified/Added)
    CHECK(snap.dir_states.at("src/app") == navigator::git::GitFileState::Modified);
    CHECK(snap.dir_states.at("src") == navigator::git::GitFileState::Renamed);
    CHECK_FALSE(snap.dir_states.contains("build")); // Ignored does not propagate to parent dir
}

TEST_CASE("git::GitStatusService caches status off-thread with fake runner",
          "[platform][p10][git]") {
    navigator::platform_test::TempDir tmp;
    std::filesystem::create_directories(tmp.path() / ".git");
    std::filesystem::create_directories(tmp.path() / "sub");

    auto runner = std::make_shared<FakeGitRunner>();
    runner->stdout_payload.append("## main");
    runner->stdout_payload.push_back('\0');
    runner->stdout_payload.append(" M sub/a.txt");
    runner->stdout_payload.push_back('\0');

    navigator::git::GitStatusService svc(runner);
    svc.request_directory(tmp.path() / "sub");

    bool updated = false;
    for (int i = 0; i < 100 && !updated; ++i) {
        updated = svc.poll();
        if (!updated) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(updated);
    CHECK(svc.branch_for(tmp.path() / "sub") == std::optional<std::string>("main"));
    CHECK(svc.status_for(tmp.path() / "sub/a.txt") == navigator::git::GitFileState::Modified);
    CHECK(svc.status_for(tmp.path() / "sub") == navigator::git::GitFileState::Modified);
    CHECK(runner->calls == 1);
}
