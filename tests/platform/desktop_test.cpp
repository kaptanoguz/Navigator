// open_with_default (fake IAppLauncher only: never starts xdg-open), mime_type on real temp
// entries, user_dir with XDG variables set/unset (including the user-dirs.dirs directories), and
// the later-phase stubs.
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <navigator/mime.hpp>
#include <navigator/platform.hpp>

#include "test_support.hpp"

namespace {

using navigator::Error;
using navigator::ErrorCode;
using navigator::Path;
using navigator::Result;
using navigator::platform::IAppLauncher;
using navigator::platform::LaunchRequest;
using navigator::platform::UserDir;
using navigator::platform_test::ScopedEnv;
using navigator::platform_test::TempDir;
using navigator::platform_test::write_file;
namespace stdfs = std::filesystem;
namespace mime = navigator::mime;
namespace platform = navigator::platform;

// Records every request; answers from a script of results (ok once the script runs out).
// Single-threaded.
class FakeLauncher final : public IAppLauncher {
public:
    [[nodiscard]] Result<void> launch(const LaunchRequest& request) override {
        requests.push_back(request);
        if (script.empty()) {
            return navigator::ok();
        }
        auto next = std::move(script.front());
        script.pop_front();
        if (next) {
            return navigator::fail(next->code, next->message);
        }
        return navigator::ok();
    }

    void then_ok() { script.emplace_back(std::nullopt); }
    void then_fail(ErrorCode code) { script.emplace_back(Error{code, "fake"}); }

    std::vector<LaunchRequest> requests;
    std::deque<std::optional<Error>> script;
};

using Argv = std::vector<std::string>;

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

TEST_CASE("open_with_default: launches xdg-open with the absolute path", "[open]") {
    TempDir dir;
    const Path file = dir / "doc with spaces.pdf";
    write_file(file, "%PDF");
    FakeLauncher fake;
    REQUIRE(platform::open_with_default(file, fake).has_value());
    REQUIRE(fake.requests.size() == 1);
    CHECK(fake.requests[0].argv == Argv{"xdg-open", file.native()});
    CHECK(fake.requests[0].working_dir.empty());

    // Directories exist too (the ui never sends them, but the contract only requires existence).
    fake.requests.clear();
    REQUIRE(platform::open_with_default(dir.path(), fake).has_value());
    CHECK(fake.requests.at(0).argv == Argv{"xdg-open", dir.path().native()});
}

TEST_CASE("open_with_default: gio fallback only when xdg-open is NotFound", "[open]") {
    TempDir dir;
    const Path file = dir / "a.txt";
    write_file(file, "a");

    SECTION("xdg-open missing, gio works") {
        FakeLauncher fake;
        fake.then_fail(ErrorCode::NotFound);
        fake.then_ok();
        REQUIRE(platform::open_with_default(file, fake).has_value());
        REQUIRE(fake.requests.size() == 2);
        CHECK(fake.requests[0].argv == Argv{"xdg-open", file.native()});
        CHECK(fake.requests[1].argv == Argv{"gio", "open", file.native()});
    }
    SECTION("neither found: Unsupported") {
        FakeLauncher fake;
        fake.then_fail(ErrorCode::NotFound);
        fake.then_fail(ErrorCode::NotFound);
        const auto result = platform::open_with_default(file, fake);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::Unsupported);
        CHECK(fake.requests.size() == 2);
    }
    SECTION("xdg-open spawn failure is reported, no fallback") {
        FakeLauncher fake;
        fake.then_fail(ErrorCode::PlatformError);
        const auto result = platform::open_with_default(file, fake);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::PlatformError);
        CHECK(fake.requests.size() == 1);
    }
    SECTION("gio spawn failure is reported as is") {
        FakeLauncher fake;
        fake.then_fail(ErrorCode::NotFound);
        fake.then_fail(ErrorCode::PlatformError);
        const auto result = platform::open_with_default(file, fake);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::PlatformError);
        CHECK(fake.requests.size() == 2);
    }
}

TEST_CASE("open_with_default: path errors never reach the launcher", "[open]") {
    TempDir dir;
    write_file(dir / "file", "x");
    FakeLauncher fake;
    const auto code = [&](const Path& path) {
        const auto result = platform::open_with_default(path, fake);
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    CHECK(code(Path()) == ErrorCode::InvalidArgument);
    CHECK(code("relative.txt") == ErrorCode::InvalidArgument);
    CHECK(code("-n") == ErrorCode::InvalidArgument); // could be parsed as an option
    CHECK(code(dir / "missing") == ErrorCode::NotFound);
    CHECK(code(dir / "file" / "child") == ErrorCode::NotFound); // ENOTDIR
    stdfs::create_symlink(dir / "missing", dir / "dangling");
    CHECK(code(dir / "dangling") == ErrorCode::NotFound); // stat follows links
    if (::geteuid() != 0) {
        stdfs::create_directories(dir / "locked");
        write_file(dir / "locked" / "inner", "x");
        stdfs::permissions(dir / "locked", stdfs::perms::none);
        CHECK(code(dir / "locked" / "inner") == ErrorCode::PermissionDenied);
        stdfs::permissions(dir / "locked", stdfs::perms::owner_all);
    }
    CHECK(fake.requests.empty());

    // The default-launcher overload validates the same way before spawning anything.
    CHECK(platform::open_with_default("relative.txt").error().code == ErrorCode::InvalidArgument);
    CHECK(platform::open_with_default(dir / "missing").error().code == ErrorCode::NotFound);
}

TEST_CASE("mime_type: lstat/stat classification of real entries", "[mime]") {
    TempDir dir;
    write_file(dir / "photo.PNG", "");
    write_file(dir / "README", "");
    write_file(dir / "run", "#!/bin/sh\n");
    stdfs::permissions(dir / "run", stdfs::perms::owner_all);
    write_file(dir / "backup.tar.gz", "");
    stdfs::create_directories(dir / "folder.png");
    stdfs::create_symlink(dir / "photo.PNG", dir / "link.txt");    // link name decides
    stdfs::create_symlink(dir / "run", dir / "runlink");           // target's exec bits
    stdfs::create_symlink(dir / "README", dir / "readmelink");     // target not executable
    stdfs::create_symlink(dir / "folder.png", dir / "folderlink"); // link to directory
    stdfs::create_symlink(dir / "nothing", dir / "dangling.png");  // dangling
    REQUIRE(::mkfifo((dir / "pipe").c_str(), 0600) == 0);

    const auto type = [](const Path& path) {
        auto result = platform::mime_type(path);
        REQUIRE(result.has_value());
        return *result;
    };
    CHECK(type(dir / "photo.PNG") == "image/png");
    CHECK(type(dir / "README") == mime::kOctetStream);
    CHECK(type(dir / "run") == mime::kExecutable);
    CHECK(type(dir / "backup.tar.gz") == "application/x-compressed-tar");
    CHECK(type(dir / "folder.png") == mime::kDirectory);
    CHECK(type(dir.path()) == mime::kDirectory);
    CHECK(type(dir / "link.txt") == "text/plain");
    CHECK(type(dir / "runlink") == mime::kExecutable);
    CHECK(type(dir / "readmelink") == mime::kOctetStream);
    CHECK(type(dir / "folderlink") == mime::kDirectory);
    CHECK(type(dir / "dangling.png") == mime::kSymlink);
    CHECK(type(dir / "pipe") == mime::kFifo);
    CHECK(type("/dev/null") == mime::kCharDevice);

    CHECK(platform::mime_type(Path()).error().code == ErrorCode::InvalidArgument);
    CHECK(platform::mime_type(dir / "missing.png").error().code == ErrorCode::NotFound);
    CHECK(platform::mime_type(dir / "README" / "x").error().code == ErrorCode::NotFound);
}

TEST_CASE("mime_type: unix socket", "[mime]") {
    TempDir dir;
    const Path sock_path = dir / "s.sock";
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    REQUIRE(sock_path.native().size() < sizeof addr.sun_path);
    sock_path.native().copy(addr.sun_path, sock_path.native().size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
    const auto result = platform::mime_type(sock_path);
    ::close(fd);
    REQUIRE(result.has_value());
    CHECK(*result == mime::kSocket);
}

TEST_CASE("user_dir: Home from $HOME, else passwd", "[user_dir]") {
    EnvGuard home("HOME");
    home.set("/custom/home");
    CHECK(platform::user_dir(UserDir::Home).value() == Path("/custom/home"));

    // Relative or empty $HOME is ignored in favour of the passwd entry (absolute).
    home.set("relative/home");
    auto fallback = platform::user_dir(UserDir::Home);
    REQUIRE(fallback.has_value());
    CHECK(fallback->is_absolute());
    home.set("");
    CHECK(platform::user_dir(UserDir::Home).value() == *fallback);
    home.unset();
    CHECK(platform::user_dir(UserDir::Home).value() == *fallback);
}

TEST_CASE("user_dir: Config and Cache honour absolute XDG variables only", "[user_dir]") {
    EnvGuard home("HOME");
    EnvGuard config("XDG_CONFIG_HOME");
    EnvGuard cache("XDG_CACHE_HOME");
    home.set("/h");

    config.unset();
    cache.unset();
    CHECK(platform::user_dir(UserDir::Config).value() == Path("/h/.config"));
    CHECK(platform::user_dir(UserDir::Cache).value() == Path("/h/.cache"));

    config.set("/xdg/config");
    cache.set("/xdg/cache");
    CHECK(platform::user_dir(UserDir::Config).value() == Path("/xdg/config"));
    CHECK(platform::user_dir(UserDir::Cache).value() == Path("/xdg/cache"));

    config.set("relative/config"); // the XDG spec says relative values are invalid
    cache.set("");
    CHECK(platform::user_dir(UserDir::Config).value() == Path("/h/.config"));
    CHECK(platform::user_dir(UserDir::Cache).value() == Path("/h/.cache"));

    // Read on every call: a later change is seen without caching.
    home.set("/other");
    CHECK(platform::user_dir(UserDir::Cache).value() == Path("/other/.cache"));
}

TEST_CASE("user_dir: XDG user directories from $XDG_CONFIG_HOME/user-dirs.dirs", "[user_dir]") {
    TempDir dir;
    EnvGuard home("HOME");
    EnvGuard config("XDG_CONFIG_HOME");
    const Path home_path = dir / "home";
    home.set(home_path.native());
    const auto dir_of = [](UserDir which) { return platform::user_dir(which); };

    SECTION("no file: Desktop default only, the rest NotFound") {
        config.set((dir / "noconfig").native());
        CHECK(dir_of(UserDir::Desktop).value() == home_path / "Desktop");
        for (const UserDir which : {UserDir::Documents, UserDir::Downloads, UserDir::Music,
                                    UserDir::Pictures, UserDir::Videos}) {
            CHECK(dir_of(which).error().code == ErrorCode::NotFound);
        }
    }
    SECTION("XDG_CONFIG_HOME set: its file, localized names") {
        config.set((dir / "cfg").native());
        write_file(dir / "cfg" / "user-dirs.dirs", "XDG_DESKTOP_DIR=\"$HOME/Masaüstü\"\n"
                                                   "XDG_DOCUMENTS_DIR=\"$HOME/Belgeler\"\n"
                                                   "XDG_DOWNLOAD_DIR=\"$HOME/İndirilenler\"\n"
                                                   "XDG_MUSIC_DIR=\"$HOME/Müzik\"\n"
                                                   "XDG_PICTURES_DIR=\"$HOME/Resimler\"\n"
                                                   "XDG_VIDEOS_DIR=\"/srv/Videolar\"\n");
        CHECK(dir_of(UserDir::Desktop).value() == home_path / "Masaüstü");
        CHECK(dir_of(UserDir::Documents).value() == home_path / "Belgeler");
        CHECK(dir_of(UserDir::Downloads).value() == home_path / "İndirilenler");
        CHECK(dir_of(UserDir::Music).value() == home_path / "Müzik");
        CHECK(dir_of(UserDir::Pictures).value() == home_path / "Resimler");
        CHECK(dir_of(UserDir::Videos).value() == Path("/srv/Videolar"));
    }
    SECTION("XDG_CONFIG_HOME unset or relative: Home/.config") {
        write_file(home_path / ".config" / "user-dirs.dirs", "XDG_MUSIC_DIR=\"$HOME/Tunes\"\n");
        config.unset();
        CHECK(dir_of(UserDir::Music).value() == home_path / "Tunes");
        config.set("relative/cfg");
        CHECK(dir_of(UserDir::Music).value() == home_path / "Tunes");
    }
    SECTION("a directory set to $HOME (disabled) returns Home as user_dir(Home) does") {
        config.set((dir / "cfg").native());
        write_file(dir / "cfg" / "user-dirs.dirs",
                   "XDG_DESKTOP_DIR=\"$HOME/\"\nXDG_DOWNLOAD_DIR=\"$HOME\"\n");
        CHECK(dir_of(UserDir::Desktop).value() == platform::user_dir(UserDir::Home).value());
        CHECK(dir_of(UserDir::Downloads).value() == home_path);
        // A trailing slash in $HOME is kept on the Home value, and still detected.
        home.set(home_path.native() + "/");
        CHECK(dir_of(UserDir::Downloads).value().native() == home_path.native() + "/");
    }
    SECTION("read on every call") {
        config.set((dir / "cfg").native());
        write_file(dir / "cfg" / "user-dirs.dirs", "XDG_PICTURES_DIR=\"$HOME/A\"\n");
        CHECK(dir_of(UserDir::Pictures).value() == home_path / "A");
        write_file(dir / "cfg" / "user-dirs.dirs", "XDG_PICTURES_DIR=\"$HOME/B\"\n");
        CHECK(dir_of(UserDir::Pictures).value() == home_path / "B");
    }
    SECTION("unreadable file: the read error") {
        if (::geteuid() != 0) {
            config.set((dir / "cfg").native());
            write_file(dir / "cfg" / "user-dirs.dirs", "XDG_MUSIC_DIR=\"$HOME/M\"\n");
            stdfs::permissions(dir / "cfg" / "user-dirs.dirs", stdfs::perms::none);
            CHECK(dir_of(UserDir::Music).error().code == ErrorCode::PermissionDenied);
        }
    }
}

TEST_CASE("open_terminal validates directory path (implemented in Phase 10)", "[stubs]") {
    CHECK(platform::open_terminal("relative").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("move_to_trash of a missing path is NotFound and creates nothing", "[trash]") {
    // Phase 7: move_to_trash is the trash.hpp wrapper (trash_test.cpp covers it fully). The
    // home trash is pointed into a fixture so the real ~/.local/share/Trash is never touched.
    TempDir dir;
    ScopedEnv data("XDG_DATA_HOME", (dir / "xdg").native());
    CHECK(platform::move_to_trash(dir / "nonexistent").error().code == ErrorCode::NotFound);
    CHECK_FALSE(stdfs::exists(dir / "xdg"));
}
