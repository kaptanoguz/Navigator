// Phase 1 acceptance: 100k synthetic entries listed + sorted (docs/perf/phase1-listing.md).
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <unistd.h>

#include <navigator/fs.hpp>

#include "temp_dir.hpp"

namespace fs = navigator::fs;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Expected name of index i; natural order equals index order.
std::string name_of(int i) { return "file-" + std::to_string(i) + ".txt"; }

} // namespace

TEST_CASE("100k entries: list and sort", "[perf][list][sort]") {
    constexpr int kCount = 100'000;
    const navigator::test::TempDir t;

    auto start = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        const std::string p = (t.path() / name_of(i)).native();
        const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        REQUIRE(fd >= 0);
        ::close(fd);
    }
    const double create_ms = ms_since(start);

    start = Clock::now();
    auto listed = fs::list_directory(t.path());
    const double list_ms = ms_since(start);
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == static_cast<std::size_t>(kCount));

    // Name, ascending (natural): file-0, file-1, ..., file-99999.
    std::vector<fs::DirEntry> by_name = *listed;
    start = Clock::now();
    fs::sort_entries(by_name, fs::SortOptions{});
    const double sort_name_ms = ms_since(start);
    bool ordered = true;
    for (int i = 0; i < kCount; ++i) {
        ordered = ordered && by_name[static_cast<std::size_t>(i)].name == name_of(i);
    }
    CHECK(ordered);
    for (const fs::DirEntry& e : by_name) {
        if (e.metadata_error || e.kind != fs::EntryKind::File || e.size != 0) {
            FAIL("unexpected entry " << e.name);
        }
    }

    // Name, descending.
    std::vector<fs::DirEntry> desc = *listed;
    fs::SortOptions d;
    d.descending = true;
    start = Clock::now();
    fs::sort_entries(desc, d);
    const double sort_desc_ms = ms_since(start);
    CHECK(desc.front().name == name_of(kCount - 1));
    CHECK(desc.back().name == name_of(0));

    // Modified (mostly equal mtimes -> falls through to the name chain).
    std::vector<fs::DirEntry> by_mtime = *listed;
    fs::SortOptions m;
    m.key = fs::SortKey::Modified;
    start = Clock::now();
    fs::sort_entries(by_mtime, m);
    const double sort_mtime_ms = ms_since(start);
    for (std::size_t i = 1; i < by_mtime.size(); ++i) {
        if (by_mtime[i - 1].mtime > by_mtime[i].mtime) {
            FAIL("mtime order broken at " << i);
        }
    }

    // Listing + sort together, as the UI worker will do it.
    start = Clock::now();
    auto again = fs::list_directory(t.path());
    REQUIRE(again.has_value());
    fs::sort_entries(*again, fs::SortOptions{});
    const double list_sort_ms = ms_since(start);
    CHECK((*again)[0].name == name_of(0));

    std::printf("[perf] 100k: create %.1f ms, list %.1f ms, sort name %.1f ms, sort name desc "
                "%.1f ms, sort mtime %.1f ms, list+sort %.1f ms\n",
                create_ms, list_ms, sort_name_ms, sort_desc_ms, sort_mtime_ms, list_sort_ms);
    std::fflush(stdout);
}
