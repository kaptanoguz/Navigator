// src/app/bench_fixture.hpp
//
// Development only (`navigator --bench-scroll N`): a generated directory with N entries
// (about 5% folders, the rest empty files with mixed names, extensions and lengths, including
// Turkish names) for the scroll benchmark. Created under $TMPDIR (or /tmp) with mkdtemp and
// removed again after the run. Never touches anything outside the directory it created.
// Thread-safety: call from one thread.
#pragma once

#include <string>
#include <vector>

#include <navigator/types.hpp>

namespace navigator::app {

struct BenchFixture {
    Path root;
    std::vector<std::string> names; // created entries, for removal
    std::vector<bool> is_dir;
};

// Names of the N generated entries (pure; exposed for tests).
[[nodiscard]] std::vector<std::string> bench_fixture_names(int count);

// Errors: IoError (mkdtemp/creat failed; partial fixtures are removed again).
[[nodiscard]] Result<BenchFixture> create_bench_fixture(int count);

// Removes what create_bench_fixture made (only those names, then the directory).
void remove_bench_fixture(const BenchFixture& fixture) noexcept;

} // namespace navigator::app
