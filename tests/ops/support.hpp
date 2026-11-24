// tests/ops/support.hpp: fixtures, guards and the fake trash for navigator_ops_tests (S16).
//
// SAFETY: every test works inside Fixture directories only (mkdtemp under
// std::filesystem::temp_directory_path(), or under /dev/shm for cross-filesystem cases).
//   - Fixture roots are registered; require_inside_fixture() ABORTS the process when a path is
//     not lexically inside a live fixture root.
//   - run() checks every path of a request before the engine sees it, and wraps
//     EngineOptions::fault_injector so every entry the engine processes is checked too.
//   - A process-wide I/O hook (installed at startup) checks every destination path the engine
//     writes, creates, commits or unlinks.
//   - FakeTrash moves items into <fixture>/trash only; it guards every path it is given.
// The real XDG trash, $HOME and the repository are never touched.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/ops.hpp>
#include <navigator/trash.hpp>

#include "sys.hpp"

namespace navigator::ops::test {

using navigator::Path;

// Aborts (std::abort) when `p` is not absolute or not inside a live fixture root.
void require_inside_fixture(const Path& p);

class Fixture {
public:
    enum class Where : std::uint8_t { Temp, Shm };
    explicit Fixture(Where where = Where::Temp);
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture();

    // True when a /dev/shm fixture would be on another filesystem than a temp fixture.
    static bool shm_is_other_filesystem();

    [[nodiscard]] const Path& root() const noexcept { return root_; }
    [[nodiscard]] Path operator/(std::string_view rel) const { return root_ / std::string(rel); }

    // Creates parent folders as needed. Names are raw bytes (invalid UTF-8 allowed).
    Path file(std::string_view rel, std::string_view content = {}) const;
    Path dir(std::string_view rel) const;
    Path link(std::string_view rel, std::string_view target) const;

private:
    Path root_;
};

// File helpers (paths are guarded).
[[nodiscard]] std::string read_file(const Path& p);
void write_file(const Path& p, std::string_view content);
[[nodiscard]] bool present(const Path& p); // lstat
[[nodiscard]] struct stat lstat_of(const Path& p);

// Deterministic digest of a tree (names, kinds, permission bits, sizes, file contents, link
// targets), never following links. Equal digests = equal trees.
[[nodiscard]] std::string tree_digest(const Path& root);
// Paths below `root` whose name looks like an engine temp file (".*.navigator-*.part").
[[nodiscard]] std::vector<Path> part_files(const Path& root);
// Number of entries below `root` (not counting root).
[[nodiscard]] std::size_t count_entries(const Path& root);

// Test I/O hook chained after the guard (cleared by ScopedIoHook's destructor).
class ScopedIoHook {
public:
    explicit ScopedIoHook(detail::IoFaultHook hook);
    ScopedIoHook(const ScopedIoHook&) = delete;
    ScopedIoHook& operator=(const ScopedIoHook&) = delete;
    ScopedIoHook(ScopedIoHook&&) = delete;
    ScopedIoHook& operator=(ScopedIoHook&&) = delete;
    ~ScopedIoHook();
};

class ScopedFreeSpace {
public:
    explicit ScopedFreeSpace(std::uint64_t bytes);
    ScopedFreeSpace(const ScopedFreeSpace&) = delete;
    ScopedFreeSpace& operator=(const ScopedFreeSpace&) = delete;
    ScopedFreeSpace(ScopedFreeSpace&&) = delete;
    ScopedFreeSpace& operator=(ScopedFreeSpace&&) = delete;
    ~ScopedFreeSpace();
};

// Fixture-local trash. Items go to <root>/files/<name> with <root>/info/<name>.trashinfo.
class FakeTrash final : public trash::ITrash {
public:
    explicit FakeTrash(Path root);

    Result<trash::TrashDir> trash_dir_for(const Path& path) override;
    Result<trash::TrashedItem> trash(const Path& path) override;
    Result<std::vector<trash::TrashDir>> trash_dirs() override;
    Result<trash::TrashListing> list(std::stop_token stop) override;
    Result<void> restore(const trash::TrashedItem& item, const Path& destination) override;
    Result<void> forget(const trash::TrashedItem& item) override;
    [[nodiscard]] bool is_inside_trash(const Path& path) const override;

    // Paths at or below these are "on a filesystem without a usable trash" (Unsupported).
    void set_unsupported(std::vector<Path> prefixes);
    [[nodiscard]] const Path& root() const noexcept { return dir_.root; }
    [[nodiscard]] std::size_t item_count() const; // entries in files/
    std::atomic<int> trash_calls{0};
    std::atomic<int> restore_calls{0};

private:
    [[nodiscard]] bool unsupported(const Path& p) const;
    mutable std::mutex mu_;
    trash::TrashDir dir_;
    std::vector<Path> unsupported_;
};

// Engine environment over one fixture.
struct Env {
    explicit Env(Fixture& fixture, bool with_trash = true);
    Fixture& fx;
    std::shared_ptr<FakeTrash> trash;
    EngineOptions engine;
};

// Runs `request` after guarding all of its paths; EngineOptions::fault_injector is wrapped with
// the guard (and calls `inject` when set).
[[nodiscard]] Result<OpResult> run(Env& env, const OpRequest& request,
                                   const ConflictResolver& resolve = {}, std::stop_token stop = {},
                                   std::function<bool(const Path&)> inject = {},
                                   const ProgressSink& progress = {});

// Guards every path of a request (sources, destination, trashed items, record steps).
void guard_request(const OpRequest& request);

// Request builders.
[[nodiscard]] OpRequest copy_req(std::vector<Path> sources, Path dest,
                                 ConflictPolicy policy = ConflictPolicy::Ask);
[[nodiscard]] OpRequest move_req(std::vector<Path> sources, Path dest,
                                 ConflictPolicy policy = ConflictPolicy::Ask);
[[nodiscard]] OpRequest rename_req(Path source, std::string new_name,
                                   ConflictPolicy policy = ConflictPolicy::Ask);
[[nodiscard]] OpRequest mkdir_req(Path parent, std::string name,
                                  ConflictPolicy policy = ConflictPolicy::Ask);
[[nodiscard]] OpRequest trash_req(std::vector<Path> sources);
[[nodiscard]] OpRequest delete_req(std::vector<Path> sources);
[[nodiscard]] OpRequest undo_req(const UndoRecord& record,
                                 ConflictPolicy policy = ConflictPolicy::Ask);
[[nodiscard]] OpRequest redo_req(const UndoRecord& record,
                                 ConflictPolicy policy = ConflictPolicy::Ask);

// Accessors that throw (failing the test) instead of dereferencing an empty optional.
[[nodiscard]] ErrorCode code_of(const ItemOutcome& o); // ErrorCode::Unknown without an error
[[nodiscard]] const Error& error_of(const ItemOutcome& o);
[[nodiscard]] const UndoRecord& record_of(const OpResult& r);
[[nodiscard]] const trash::TrashedItem& trashed_of(const UndoStep& s);
[[nodiscard]] const trash::TrashedItem& replaced_of(const UndoStep& s);

// A resolver that always answers `decision` and counts the conflicts it saw.
struct Answer {
    explicit Answer(ConflictDecision d) : decision(std::move(d)) {}
    ConflictDecision decision;
    std::vector<Conflict> seen;
    ConflictResolver resolver();
};

} // namespace navigator::ops::test
