// Throughput measurements for docs/perf/phase7-ops.md. Hidden ([.perf]): run explicitly with
//   navigator_ops_tests "[.perf]"
// Everything happens inside fixtures (deleted afterwards); about 2 GiB of disk is used at peak.
#include <chrono>
#include <cstdio>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;
using Clock = std::chrono::steady_clock;

namespace {

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

void make_big(const Path& p, std::size_t bytes) {
    require_inside_fixture(p);
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    REQUIRE(fd >= 0);
    std::string chunk(1 << 20, '\0');
    for (std::size_t i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<char>((i * 2654435761U) >> 13U);
    }
    for (std::size_t done = 0; done < bytes; done += chunk.size()) {
        REQUIRE(::write(fd, chunk.data(), chunk.size()) == static_cast<ssize_t>(chunk.size()));
    }
    ::fsync(fd);
    ::close(fd);
}

void report(const char* what, double secs, double mib, std::size_t items) {
    std::printf("PERF %-44s %8.3f s  %9.1f MiB/s  %9.0f items/s\n", what, secs,
                secs > 0 ? mib / secs : 0.0, secs > 0 ? static_cast<double>(items) / secs : 0.0);
    std::fflush(stdout);
}

} // namespace

TEST_CASE("perf: 1 GiB file", "[.perf]") {
    constexpr std::size_t kBytes = std::size_t{1} << 30;
    Fixture fx;
    Env env(fx);
    env.engine.copy_chunk_bytes = std::size_t{1} << 20;
    make_big(fx / "big.bin", kBytes);
    fx.dir("dst");
    {
        auto t0 = Clock::now();
        auto r = run(env, copy_req({fx / "big.bin"}, fx / "dst"));
        const double s = seconds_since(t0);
        REQUIRE(r);
        REQUIRE(r->done == 1);
        report("copy 1 GiB, same fs (page cache warm)", s, 1024.0, 1);
    }
    std::filesystem::remove(fx / "dst/big.bin");
    {
        OpRequest req = copy_req({fx / "big.bin"}, fx / "dst");
        req.options.sync_copies = true;
        auto t0 = Clock::now();
        auto r = run(env, req);
        const double s = seconds_since(t0);
        REQUIRE(r);
        report("copy 1 GiB, same fs, sync_copies (fsync)", s, 1024.0, 1);
    }
    std::filesystem::remove(fx / "dst/big.bin");
    {
        // Baseline: coreutils cp on the same fixture paths.
        const std::string cmd = "cp --reflink=never '" + (fx / "big.bin").native() + "' '" +
                                (fx / "dst/cp.bin").native() + "'";
        require_inside_fixture(fx / "dst/cp.bin");
        auto t0 = Clock::now();
        REQUIRE(std::system(cmd.c_str()) == 0);
        report("baseline cp 1 GiB, same fs", seconds_since(t0), 1024.0, 1);
        std::filesystem::remove(fx / "dst/cp.bin");
    }
    if (Fixture::shm_is_other_filesystem()) {
        Fixture shm(Fixture::Where::Shm);
        auto t0 = Clock::now();
        auto r = run(env, copy_req({fx / "big.bin"}, shm.root()));
        REQUIRE(r);
        report("copy 1 GiB, disk -> tmpfs", seconds_since(t0), 1024.0, 1);
        std::filesystem::remove(shm / "big.bin");
        t0 = Clock::now();
        auto m = run(env, move_req({fx / "big.bin"}, shm.root()));
        REQUIRE(m);
        REQUIRE(m->done == 1);
        report("move 1 GiB, disk -> tmpfs (fsync+verify+unlink)", seconds_since(t0), 1024.0, 1);
    }
}

TEST_CASE("perf: 50k small files", "[.perf]") {
    constexpr int kDirs = 100;
    constexpr int kPerDir = 500;
    constexpr std::size_t kTotal = static_cast<std::size_t>(kDirs) * kPerDir;
    Fixture fx;
    Env env(fx);
    const std::string body(1024, 'q');
    for (int d = 0; d < kDirs; ++d) {
        const Path dir = fx.dir("src/t/d" + std::to_string(d));
        for (int f = 0; f < kPerDir; ++f) {
            write_file(dir / ("f" + std::to_string(f) + ".txt"), body);
        }
    }
    const double mib = static_cast<double>(kTotal) * 1024.0 / (1024.0 * 1024.0);
    fx.dir("dst");
    {
        auto t0 = Clock::now();
        auto r = run(env, copy_req({fx / "src/t"}, fx / "dst"));
        const double s = seconds_since(t0);
        REQUIRE(r);
        REQUIRE(r->done == 1);
        REQUIRE_FALSE(r->items[0].partial);
        report("copy 50k x 1 KiB files, same fs", s, mib, kTotal);
    }
    {
        const std::string cmd =
            "cp -a '" + (fx / "src/t").native() + "' '" + (fx / "dst/cp").native() + "'";
        require_inside_fixture(fx / "dst/cp");
        auto t0 = Clock::now();
        REQUIRE(std::system(cmd.c_str()) == 0);
        report("baseline cp -a 50k files", seconds_since(t0), mib, kTotal);
    }
    {
        auto t0 = Clock::now();
        auto r = run(env, trash_req({fx / "dst/cp"}));
        REQUIRE(r);
        report("trash a 50k-file folder (one rename)", seconds_since(t0), 0, 1);
    }
    {
        auto t0 = Clock::now();
        auto r = run(env, delete_req({fx / "dst/t"}));
        const double s = seconds_since(t0);
        REQUIRE(r);
        REQUIRE(r->done == 1);
        report("permanent delete 50k files (scan + unlink)", s, 0, kTotal);
    }
    {
        ScopedIoHook hook([](ops::detail::IoSite site, const Path&, std::uint64_t) -> int {
            return site == ops::detail::IoSite::Rename ? EXDEV : 0;
        });
        auto t0 = Clock::now();
        auto r = run(env, move_req({fx / "src/t"}, fx / "dst"));
        const double s = seconds_since(t0);
        REQUIRE(r);
        REQUIRE(r->done == 1);
        report("cross-fs move path, 50k files (fsync each)", s, mib, kTotal);
    }
}
