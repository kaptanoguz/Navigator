// The default proc IMountSource, on the real /proc and on fixture files.
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <navigator/volume.hpp>

#include "test_support.hpp"

using navigator::platform_test::TempDir;
using navigator::platform_test::write_file;

TEST_CASE("proc source reads /proc/self/mountinfo", "[volume][proc]") {
    auto source = navigator::create_proc_mount_source();
    REQUIRE(source.has_value());
    CHECK((*source)->change_fd() >= 0);
    auto text = (*source)->read_mountinfo();
    REQUIRE(text.has_value());
    CHECK_FALSE(navigator::parse_mountinfo(*text).empty());

    auto space = (*source)->query_space("/");
    REQUIRE(space.has_value());
    CHECK(space->total_bytes > 0);
    CHECK(space->free_bytes >= space->available_bytes);
    CHECK(space->total_bytes >= space->free_bytes);

    auto missing = (*source)->query_space("/nonexistent-navigator-mount");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == navigator::ErrorCode::NotFound);
}

TEST_CASE("proc source on a fixture file re-reads it each time", "[volume][proc]") {
    TempDir tmp;
    const auto file = tmp / "mountinfo";
    write_file(file, "22 1 8:1 / / rw - ext4 /dev/sda1 rw\n");
    auto source = navigator::create_proc_mount_source(file);
    REQUIRE(source.has_value());
    auto first = (*source)->read_mountinfo();
    REQUIRE(first.has_value());
    CHECK(*first == "22 1 8:1 / / rw - ext4 /dev/sda1 rw\n");

    std::string big;
    for (int i = 0; i < 2000; ++i) { // larger than one read chunk
        big += "22 1 8:1 / /m" + std::to_string(i) + " rw - ext4 /dev/sda1 rw\n";
    }
    write_file(file, big);
    auto second = (*source)->read_mountinfo();
    REQUIRE(second.has_value());
    CHECK(*second == big);
}

TEST_CASE("proc source creation errors", "[volume][proc]") {
    auto missing = navigator::create_proc_mount_source("/nonexistent-navigator/mountinfo");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == navigator::ErrorCode::NotFound);
}
