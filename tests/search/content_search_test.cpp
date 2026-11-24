#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/content_search.hpp>

#include "temp_dir.hpp"

namespace {

using navigator::Path;
using navigator::search::ContentMatch;
using navigator::search::ContentSearchOptions;
using navigator::search::ContentSearchSession;

void write_text(const Path& p, std::string_view data) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

} // namespace

TEST_CASE("search::content_search finds matches, skips binary and large files",
          "[search][p10][content]") {
    navigator::test::TempDir tmp;
    write_text(tmp.path() / "a.txt", "Hello World\nSecond line with NEEDLE here\nThird line\n");
    write_text(tmp.path() / "sub/b.cpp", "int needle = 42;\n// another needle\n");

    // Binary file containing NUL byte + "needle"
    std::string bin("ELF\0needle\0binary", 17);
    write_text(tmp.path() / "bin.dat", bin);

    // Large file exceeding cap
    write_text(tmp.path() / "huge.txt", std::string(4096, 'x') + "needle");

    ContentSearchOptions opts;
    opts.root = tmp.path();
    opts.query = "needle";
    opts.case_sensitive = false;
    opts.skip_binary = true;
    opts.max_file_bytes = 1024;

    ContentSearchSession session;
    session.start(opts);

    std::vector<ContentMatch> all;
    navigator::search::ContentSearchStats final_stats;
    for (int i = 0; i < 200; ++i) {
        auto poll = session.poll();
        for (auto& m : poll.new_matches) {
            all.push_back(std::move(m));
        }
        final_stats = poll.stats;
        if (final_stats.done) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(final_stats.done);
    CHECK_FALSE(final_stats.cancelled);
    CHECK(final_stats.files_skipped_binary == 1);
    CHECK(final_stats.files_skipped_large == 1);
    CHECK(all.size() == 3);
}

TEST_CASE("search::content_search respects case sensitivity and cancellation",
          "[search][p10][content]") {
    navigator::test::TempDir tmp;
    for (int i = 0; i < 40; ++i) {
        write_text(tmp.path() / ("f" + std::to_string(i) + ".txt"), "Alpha\nalpha\nALPHA\n");
    }

    ContentSearchOptions opts;
    opts.root = tmp.path();
    opts.query = "Alpha";
    opts.case_sensitive = true;

    std::vector<ContentMatch> hits;
    auto scanned =
        navigator::search::search_file_content(tmp.path() / "f0.txt", "f0.txt", opts, hits);
    REQUIRE(scanned.has_value());
    CHECK(*scanned == true);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].line_number == 1);

    // Pre-cancelled stop_token returns Cancelled.
    std::stop_source ss;
    ss.request_stop();
    auto cancelled = navigator::search::search_file_content(tmp.path() / "f0.txt", "f0.txt", opts,
                                                            hits, ss.get_token());
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == navigator::ErrorCode::Cancelled);
}
