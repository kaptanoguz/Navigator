// fs::list_directory() under concurrent mutation and cross-thread cancellation (owner: qa).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/fs.hpp>

#include "fixture.hpp"

namespace {

using navigator::ErrorCode;
using navigator::Path;
using navigator::Result;
using navigator::fs::DirEntry;
using navigator::fs::EntryKind;
using navigator::fs::ListOptions;
using navigator::integration_test::is_documented_list_error;
using navigator::integration_test::is_documented_metadata_error;
using navigator::integration_test::is_sane_name;
using navigator::integration_test::TempDir;
using navigator::integration_test::touch;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kPool = 256; // names "n0".."n255" (and ".h<k>" hidden variants)

std::string pool_name(std::uint32_t k) { return (k % 7U == 0U ? ".h" : "n") + std::to_string(k); }

// Random create / delete / rename / mkdir / rmdir / symlink inside `dir` until stopped.
void mutate(const Path& dir, std::uint32_t seed, const std::stop_token& stop,
            std::atomic<std::uint64_t>& ops) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::uint32_t> pick(0, kPool - 1);
    std::uniform_int_distribution<int> op(0, 5);
    while (!stop.stop_requested()) {
        const Path a = dir / pool_name(pick(rng));
        const Path b = dir / pool_name(pick(rng));
        switch (op(rng)) {
        case 0: (void)touch(a); break;
        case 1: ::unlink(a.c_str()); break;
        case 2: ::rename(a.c_str(), b.c_str()); break;
        case 3: ::mkdir(a.c_str(), 0755); break;
        case 4: ::rmdir(a.c_str()); break;
        default: {
            // Fails when `a` exists; the result is irrelevant (warn_unused_result under FORTIFY).
            [[maybe_unused]] const int rc = ::symlink(b.filename().c_str(), a.c_str());
            break;
        }
        }
        ops.fetch_add(1, std::memory_order_relaxed);
    }
}

struct ListingStats {
    std::size_t calls = 0;
    std::size_t ok = 0;
    std::size_t entries = 0;
    std::size_t metadata_errors = 0;
    std::size_t duplicate_names = 0; // same name twice in one listing (allowed by readdir)
};

// Everything a successful listing under mutation may and may not contain.
void check_entries(const std::vector<DirEntry>& entries, ListingStats& stats) {
    std::vector<std::string_view> names;
    names.reserve(entries.size());
    for (const DirEntry& entry : entries) {
        REQUIRE(is_sane_name(entry.name));
        // Only names the writer can produce.
        const bool from_pool = entry.name.starts_with("n") || entry.name.starts_with(".h");
        REQUIRE(from_pool);
        REQUIRE(entry.hidden == entry.name.starts_with("."));
        if (entry.metadata_error) {
            ++stats.metadata_errors;
            REQUIRE(is_documented_metadata_error(*entry.metadata_error));
            // Metadata fields stay at their defaults.
            REQUIRE(entry.size == 0);
            REQUIRE(entry.mode == 0);
            REQUIRE(entry.id == navigator::FileId{});
        } else {
            REQUIRE(entry.kind != EntryKind::Unknown);
            REQUIRE(entry.mode != 0);
            REQUIRE(entry.id.inode != 0);
            if (entry.kind != EntryKind::Symlink) {
                REQUIRE(entry.target_kind == EntryKind::Unknown);
                REQUIRE_FALSE(entry.dangling);
            } else {
                REQUIRE(entry.target_kind != EntryKind::Symlink);
            }
        }
        names.push_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    stats.duplicate_names +=
        static_cast<std::size_t>(names.end() - std::unique(names.begin(), names.end()));
    stats.entries += entries.size();
}

} // namespace

TEST_CASE("list_directory while another thread mutates the directory", "[integration][race][fs]") {
    const TempDir tmp;
    const Path& dir = tmp.path();
    for (std::uint32_t k = 0; k < kPool; k += 2) {
        (void)touch(dir / pool_name(k));
    }

    std::atomic<std::uint64_t> ops{0};
    ListingStats stats;
    {
        std::jthread writer_a([&](const std::stop_token& stop) { mutate(dir, 1U, stop, ops); });
        std::jthread writer_b([&](const std::stop_token& stop) { mutate(dir, 2U, stop, ops); });

        const auto deadline = Clock::now() + std::chrono::milliseconds(1500);
        while (Clock::now() < deadline) {
            ListOptions options;
            options.show_hidden = (stats.calls % 2) == 0;
            options.follow_symlinks = (stats.calls % 3) == 0;
            Result<std::vector<DirEntry>> listed = navigator::fs::list_directory(dir, options);
            ++stats.calls;
            if (!listed) {
                // The directory itself never disappears in this test.
                FAIL("list_directory failed: " << listed.error().message);
            }
            ++stats.ok;
            if (!options.show_hidden) {
                for (const DirEntry& entry : *listed) {
                    REQUIRE_FALSE(entry.hidden);
                }
            }
            check_entries(*listed, stats);
        }
    }
    INFO("calls=" << stats.calls << " entries=" << stats.entries
                  << " metadata_errors=" << stats.metadata_errors
                  << " duplicates=" << stats.duplicate_names << " writer_ops=" << ops.load());
    CHECK(stats.calls > 10);
    CHECK(ops.load() > 1000);
    WARN("mutate: calls=" << stats.calls << " entries=" << stats.entries << " metadata_errors="
                          << stats.metadata_errors << " duplicates=" << stats.duplicate_names
                          << " writer_ops=" << ops.load());
}

TEST_CASE("list_directory of a directory that is renamed, replaced and removed concurrently",
          "[integration][race][fs]") {
    const TempDir tmp;
    const Path victim = tmp / "victim";
    const Path aside = tmp / "aside";
    const Path file = tmp / "plainfile";

    std::atomic<std::uint64_t> ops{0};
    std::size_t ok = 0;
    std::size_t failed = 0;
    std::vector<ErrorCode> codes;
    {
        std::jthread writer([&](const std::stop_token& stop) {
            std::mt19937 rng(7U);
            std::uniform_int_distribution<int> op(0, 4);
            while (!stop.stop_requested()) {
                switch (op(rng)) {
                case 0: // fresh directory with a few files
                    if (::mkdir(victim.c_str(), 0755) == 0) {
                        for (int i = 0; i < 8; ++i) {
                            (void)touch(victim / ("n" + std::to_string(i)));
                        }
                    }
                    break;
                case 1: ::rename(victim.c_str(), aside.c_str()); break;
                case 2: ::rename(aside.c_str(), victim.c_str()); break;
                case 3: { // replace by a regular file (ENOTDIR -> InvalidArgument)
                    std::error_code ec;
                    std::filesystem::remove_all(victim, ec);
                    if (touch(file)) {
                        ::rename(file.c_str(), victim.c_str());
                    }
                    break;
                }
                default: {
                    std::error_code ec;
                    std::filesystem::remove_all(victim, ec);
                    std::filesystem::remove_all(aside, ec);
                    break;
                }
                }
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });

        const auto deadline = Clock::now() + std::chrono::milliseconds(1000);
        while (Clock::now() < deadline) {
            Result<std::vector<DirEntry>> listed = navigator::fs::list_directory(victim);
            if (listed) {
                ++ok;
                for (const DirEntry& entry : *listed) {
                    REQUIRE(is_sane_name(entry.name));
                    REQUIRE(entry.name.starts_with("n"));
                    if (entry.metadata_error) {
                        REQUIRE(is_documented_metadata_error(*entry.metadata_error));
                    }
                }
            } else {
                ++failed;
                REQUIRE(is_documented_list_error(listed.error().code));
                REQUIRE_FALSE(listed.error().message.empty());
                codes.push_back(listed.error().code);
            }
        }
    }
    const auto count = [&](ErrorCode c) { return std::count(codes.begin(), codes.end(), c); };
    INFO("ok=" << ok << " failed=" << failed << " not_found=" << count(ErrorCode::NotFound)
               << " invalid_argument=" << count(ErrorCode::InvalidArgument)
               << " writer_ops=" << ops.load());
    CHECK(ok + failed > 10);
    WARN("replace: ok=" << ok << " failed=" << failed << " not_found=" << count(ErrorCode::NotFound)
                        << " invalid_argument=" << count(ErrorCode::InvalidArgument)
                        << " writer_ops=" << ops.load());
}

TEST_CASE("list_directory cancelled from another thread: success or Cancelled, never partial",
          "[integration][race][fs][cancel]") {
    constexpr std::size_t kEntries = 50'000;
    const TempDir tmp;
    for (std::size_t i = 0; i < kEntries; ++i) {
        REQUIRE(touch(tmp / ("e" + std::to_string(i))));
    }

    // Reference timing of an uncancelled listing.
    const auto t0 = Clock::now();
    Result<std::vector<DirEntry>> full = navigator::fs::list_directory(tmp.path());
    const auto full_time = Clock::now() - t0;
    REQUIRE(full);
    REQUIRE(full->size() == kEntries);
    const auto full_us = std::chrono::duration_cast<std::chrono::microseconds>(full_time).count();

    // Stop requested before the call.
    {
        std::stop_source source;
        source.request_stop();
        Result<std::vector<DirEntry>> listed =
            navigator::fs::list_directory(tmp.path(), {}, source.get_token());
        REQUIRE_FALSE(listed);
        REQUIRE(listed.error().code == ErrorCode::Cancelled);
    }

    // Stop requested at random points during the call (one 50k fixture, no SECTIONs: a SECTION
    // would re-run the fixture setup).
    {
        constexpr int kTrials = 24;
        std::mt19937 rng(42U);
        std::uniform_int_distribution<long long> delay_us(0, std::max<long long>(2 * full_us, 1));
        int cancelled = 0;
        int completed = 0;
        long long max_latency_us = 0;
        long long sum_latency_us = 0;
        for (int trial = 0; trial < kTrials; ++trial) {
            std::stop_source source;
            std::atomic<long long> stop_at_ns{0};
            const auto delay = std::chrono::microseconds(delay_us(rng));
            Result<std::vector<DirEntry>> listed = navigator::fail(ErrorCode::Unknown);
            {
                std::jthread canceller([&] {
                    std::this_thread::sleep_for(delay);
                    stop_at_ns.store(Clock::now().time_since_epoch().count(),
                                     std::memory_order_release);
                    source.request_stop();
                });
                listed = navigator::fs::list_directory(tmp.path(), {}, source.get_token());
                const long long returned_ns = Clock::now().time_since_epoch().count();
                if (!listed) {
                    // Cancelled implies the stop was already requested; measure latency.
                    REQUIRE(listed.error().code == ErrorCode::Cancelled);
                    const long long requested = stop_at_ns.load(std::memory_order_acquire);
                    REQUIRE(requested != 0);
                    const long long latency_us = (returned_ns - requested) / 1000;
                    max_latency_us = std::max(max_latency_us, latency_us);
                    sum_latency_us += latency_us;
                    ++cancelled;
                }
            }
            if (listed) {
                // Success is always the complete listing, never a prefix.
                REQUIRE(listed->size() == kEntries);
                for (const DirEntry& entry : *listed) {
                    REQUIRE(entry.name.starts_with("e"));
                    REQUIRE_FALSE(entry.metadata_error);
                }
                ++completed;
            }
        }
        INFO("full_listing_us=" << full_us << " cancelled=" << cancelled
                                << " completed=" << completed
                                << " max_latency_us=" << max_latency_us << " mean_latency_us="
                                << (cancelled > 0 ? sum_latency_us / cancelled : 0));
        CHECK(cancelled + completed == kTrials);
        // 256 readdir results between checks: far below a frame even under sanitizers.
        CHECK(max_latency_us < 100'000);
        // Report the numbers in the test log (ctest -V) for docs/qa-phase1.md.
        WARN("cancel: full_listing_us=" << full_us << " cancelled=" << cancelled << "/" << kTrials
                                        << " max_latency_us=" << max_latency_us
                                        << " mean_latency_us="
                                        << (cancelled > 0 ? sum_latency_us / cancelled : 0));
    }
}
