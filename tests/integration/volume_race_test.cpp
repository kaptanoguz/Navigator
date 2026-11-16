// list_volumes() / volume_for() called concurrently from several threads against the real
// /proc/self/mountinfo (owner: qa). volume.hpp: both free functions are reentrant, and the
// default proc source is stateless and may be shared.
#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/volume.hpp>

#include "fixture.hpp"

namespace {

using navigator::IMountSource;
using navigator::Path;
using navigator::Result;
using navigator::VolumeInfo;
using navigator::VolumeKind;
using navigator::VolumeQuery;
using navigator::integration_test::TempDir;
using navigator::integration_test::touch;

// Component-wise prefix test on lexically normal absolute paths.
bool is_under(const Path& path, const Path& mount_point) {
    auto p = path.begin();
    for (auto m = mount_point.begin(); m != mount_point.end(); ++m, ++p) {
        if (m->empty()) {
            continue; // trailing separator
        }
        if (p == path.end() || *p != *m) {
            return false;
        }
    }
    return true;
}

struct ThreadLog {
    std::size_t list_calls = 0;
    std::size_t list_errors = 0;
    std::size_t lists_without_root = 0;
    std::size_t volume_for_calls = 0;
    std::size_t volume_for_errors = 0;
    std::size_t mismatches = 0; // differs from the single-threaded baseline
    std::string first_error;
};

struct Baseline {
    Path root_mount;
    Path tmp_mount;
    std::size_t volumes = 0;
};

void record_error(ThreadLog& log, const navigator::Error& error) {
    if (log.first_error.empty()) {
        log.first_error = std::string(navigator::to_string(error.code)) + ": " + error.message;
    }
}

void hammer(ThreadLog& log, const VolumeQuery& list_query, const VolumeQuery& for_query,
            const Path& file, const Path& missing, const Baseline& base, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        Result<std::vector<VolumeInfo>> volumes = navigator::list_volumes(list_query);
        ++log.list_calls;
        if (!volumes) {
            ++log.list_errors;
            record_error(log, volumes.error());
        } else {
            const bool root = std::any_of(volumes->begin(), volumes->end(), [](const auto& v) {
                return v.mount_point == "/" && v.kind == VolumeKind::Root;
            });
            log.lists_without_root += root ? 0U : 1U;
            // The mount table can legitimately change while the test runs; only count.
            log.mismatches += volumes->size() == base.volumes ? 0U : 1U;
        }

        const Path probes[] = {Path("/"), file, missing};
        for (const Path& probe : probes) {
            Result<VolumeInfo> vol = navigator::volume_for(probe, for_query);
            ++log.volume_for_calls;
            if (!vol) {
                ++log.volume_for_errors;
                record_error(log, vol.error());
                continue;
            }
            const Path& expected = probe == "/" ? base.root_mount : base.tmp_mount;
            log.mismatches += vol->mount_point == expected ? 0U : 1U;
        }
    }
}

} // namespace

TEST_CASE("list_volumes and volume_for from 4 threads at once", "[integration][race][volume]") {
    const TempDir tmp;
    const Path file = tmp / "probe.txt";
    REQUIRE(touch(file));
    const Path missing = tmp / "does" / "not" / "exist";

    Result<std::shared_ptr<IMountSource>> shared = navigator::create_proc_mount_source();
    REQUIRE(shared);

    VolumeQuery list_query;
    list_query.query_space = false; // statvfs of every mount (maybe a hung NFS) is not tested
    VolumeQuery for_query;          // local mounts only: space on
    for_query.query_space = true;

    Baseline base;
    {
        Result<std::vector<VolumeInfo>> volumes = navigator::list_volumes(list_query);
        REQUIRE(volumes);
        base.volumes = volumes->size();
        Result<VolumeInfo> root = navigator::volume_for("/", for_query);
        REQUIRE(root);
        REQUIRE(root->mount_point == "/");
        REQUIRE(root->kind == VolumeKind::Root);
        base.root_mount = root->mount_point;
        Result<VolumeInfo> in_tmp = navigator::volume_for(file, for_query);
        REQUIRE(in_tmp);
        REQUIRE(is_under(tmp.path(), in_tmp->mount_point));
        REQUIRE(in_tmp->has_space);
        base.tmp_mount = in_tmp->mount_point;
        // A missing path resolves through its existing prefix (weakly_canonical).
        Result<VolumeInfo> in_missing = navigator::volume_for(missing, for_query);
        REQUIRE(in_missing);
        REQUIRE(in_missing->mount_point == base.tmp_mount);
    }

    constexpr int kThreads = 4;
    constexpr int kIterations = 60;
    std::vector<ThreadLog> logs(kThreads);
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                // Threads 0-1: a default source per call; threads 2-3: one shared source.
                VolumeQuery lq = list_query;
                VolumeQuery fq = for_query;
                if (t >= 2) {
                    lq.source = *shared;
                    fq.source = *shared;
                }
                hammer(logs[static_cast<std::size_t>(t)], lq, fq, file, missing, base, kIterations);
            });
        }
    }

    for (const ThreadLog& log : logs) {
        INFO("first_error=" << log.first_error << " mismatches=" << log.mismatches);
        CHECK(log.list_calls == static_cast<std::size_t>(kIterations));
        CHECK(log.list_errors == 0);
        CHECK(log.lists_without_root == 0);
        CHECK(log.volume_for_calls == static_cast<std::size_t>(kIterations) * 3U);
        CHECK(log.volume_for_errors == 0);
        CHECK(log.mismatches == 0);
    }
}

TEST_CASE("volume_for rejects an empty path from concurrent callers",
          "[integration][race][volume]") {
    std::vector<int> codes(4, -1);
    {
        std::vector<std::jthread> threads;
        threads.reserve(codes.size());
        for (int& code : codes) {
            threads.emplace_back([&code] {
                VolumeQuery query;
                query.query_space = false;
                Result<VolumeInfo> vol = navigator::volume_for(Path{}, query);
                code = vol ? -2 : static_cast<int>(vol.error().code);
            });
        }
    }
    for (const int code : codes) {
        CHECK(code == static_cast<int>(navigator::ErrorCode::InvalidArgument));
    }
}
