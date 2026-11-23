// Phase 6 environment lookups: thumbnail_cache_dir(), data_dirs() and find_program().
// Every test sets the variables it depends on through ScopedEnv and uses temp dirs only.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <navigator/platform.hpp>

#include "test_support.hpp"

namespace {

using navigator::ErrorCode;
using navigator::Path;
using navigator::platform::data_dirs;
using navigator::platform::find_program;
using navigator::platform::thumbnail_cache_dir;
using navigator::platform_test::ScopedEnv;
using navigator::platform_test::TempDir;
using navigator::platform_test::write_file;
namespace stdfs = std::filesystem;

std::vector<Path> paths(std::initializer_list<const char*> list) {
    std::vector<Path> out;
    for (const char* item : list) {
        out.emplace_back(item);
    }
    return out;
}

void make_executable(const Path& path, stdfs::perms perms = stdfs::perms::owner_all) {
    write_file(path, "#!/bin/sh\nexit 0\n");
    stdfs::permissions(path, perms);
}

} // namespace

TEST_CASE("thumbnail_cache_dir: XDG_CACHE_HOME matrix", "[program_dirs]") {
    const ScopedEnv home("HOME", std::string("/home/tester"));
    struct Case {
        std::optional<std::string> cache_home;
        const char* expected;
    };
    const Case cases[] = {
        {std::string("/var/cache/me"), "/var/cache/me/thumbnails"},
        {std::nullopt, "/home/tester/.cache/thumbnails"},
        {std::string(""), "/home/tester/.cache/thumbnails"},
        {std::string("relative/cache"), "/home/tester/.cache/thumbnails"},
    };
    for (const Case& c : cases) {
        const ScopedEnv cache("XDG_CACHE_HOME", c.cache_home);
        auto dir = thumbnail_cache_dir();
        REQUIRE(dir.has_value());
        CHECK(*dir == Path(c.expected));
    }
}

TEST_CASE("data_dirs: XDG_DATA_HOME and XDG_DATA_DIRS matrix", "[program_dirs]") {
    const ScopedEnv home("HOME", std::string("/home/tester"));
    struct Case {
        std::optional<std::string> data_home;
        std::optional<std::string> data_dirs;
        std::vector<Path> expected;
    };
    const std::vector<Case> cases = {
        // Defaults.
        {std::nullopt, std::nullopt,
         paths({"/home/tester/.local/share", "/usr/local/share", "/usr/share"})},
        {std::string(""), std::string(""),
         paths({"/home/tester/.local/share", "/usr/local/share", "/usr/share"})},
        // Relative values are ignored.
        {std::string("rel/data"), std::string("rel:/opt/share"),
         paths({"/home/tester/.local/share", "/opt/share"})},
        // Explicit values in order; empty entries skipped; no implicit defaults appended.
        {std::string("/data/home"), std::string("/a/share::/b/share"),
         paths({"/data/home", "/a/share", "/b/share"})},
        // Duplicates (lexically normal) kept once at their first position, incl. data home.
        {std::string("/x/share/"), std::string("/usr/share:/x/./share:/usr/share/:/y/../usr/share"),
         paths({"/x/share", "/usr/share"})},
        // Flatpak-like order is kept as given.
        {std::nullopt, std::string("/var/lib/flatpak/exports/share:/usr/share:/usr/local/share"),
         paths({"/home/tester/.local/share", "/var/lib/flatpak/exports/share", "/usr/share",
                "/usr/local/share"})},
    };
    for (const Case& c : cases) {
        const ScopedEnv data_home("XDG_DATA_HOME", c.data_home);
        const ScopedEnv dirs("XDG_DATA_DIRS", c.data_dirs);
        auto result = data_dirs();
        REQUIRE(result.has_value());
        CHECK(*result == c.expected);
    }
}

TEST_CASE("find_program: PATH search follows execvp rules", "[program_dirs]") {
    const TempDir tmp;
    const Path a = tmp / "a";
    const Path b = tmp / "b";
    stdfs::create_directories(a);
    stdfs::create_directories(b);
    make_executable(b / "tool");
    write_file(a / "tool", "not executable"); // regular, no x bit: skipped
    stdfs::create_directories(a / "dirtool"); // directory with x bit: skipped
    make_executable(b / "dirtool");
    stdfs::create_symlink(b / "tool", a / "linked");        // symlink to an executable: ok
    stdfs::create_symlink(tmp / "missing", a / "dangling"); // dangling: skipped

    const ScopedEnv path("PATH", a.native() + ":" + b.native());
    auto tool = find_program("tool");
    REQUIRE(tool.has_value());
    CHECK(*tool == b / "tool");
    auto dirtool = find_program("dirtool");
    REQUIRE(dirtool.has_value());
    CHECK(*dirtool == b / "dirtool");
    auto linked = find_program("linked");
    REQUIRE(linked.has_value());
    CHECK(*linked == a / "linked");
    auto dangling = find_program("dangling");
    REQUIRE_FALSE(dangling.has_value());
    CHECK(dangling.error().code == ErrorCode::NotFound);
    auto missing = find_program("no-such-tool");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("find_program: names with a slash are checked as is", "[program_dirs]") {
    const TempDir tmp;
    make_executable(tmp / "direct");
    write_file(tmp / "plain", "x");
    const ScopedEnv path("PATH", std::string("/nonexistent"));
    auto direct = find_program((tmp / "direct").native());
    REQUIRE(direct.has_value());
    CHECK(*direct == tmp / "direct");
    auto plain = find_program((tmp / "plain").native());
    REQUIRE_FALSE(plain.has_value());
    CHECK(plain.error().code == ErrorCode::NotFound);
    auto dir = find_program(tmp.path().native());
    REQUIRE_FALSE(dir.has_value());
    CHECK(dir.error().code == ErrorCode::NotFound);
}

TEST_CASE("find_program: empty PATH entry is the current directory", "[program_dirs]") {
    const TempDir tmp;
    make_executable(tmp / "here");
    const Path old_cwd = stdfs::current_path();
    stdfs::current_path(tmp.path());
    const ScopedEnv path("PATH", std::string("/nonexistent::/also-missing"));
    auto here = find_program("here");
    stdfs::current_path(old_cwd);
    REQUIRE(here.has_value());
    CHECK(here->filename() == "here");
}

TEST_CASE("find_program: unset PATH uses confstr(_CS_PATH)", "[program_dirs]") {
    const ScopedEnv path("PATH", std::nullopt);
    auto sh = find_program("sh"); // /bin/sh or /usr/bin/sh exists on every target
    REQUIRE(sh.has_value());
    CHECK(sh->is_absolute());
}

TEST_CASE("find_program: invalid names", "[program_dirs]") {
    auto empty = find_program("");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == ErrorCode::InvalidArgument);
    auto nul = find_program(std::string_view("a\0b", 3));
    REQUIRE_FALSE(nul.has_value());
    CHECK(nul.error().code == ErrorCode::InvalidArgument);
    CHECK(nul.error().message.starts_with("find_program:"));
}
