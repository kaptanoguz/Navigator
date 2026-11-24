// navigator/git.hpp
//
// Purpose: Phase 10 read-only cached Git status overlays (branch name and per-file / per-folder
//          status badges) via `git status --porcelain=v1 -z -b` executed off-thread through
//          platform::IProcessRunner. Never blocks the UI thread and never mutates the repo.
// Owner:   contracts (header); implementation in src/platform (role: platform).
// Thread-safety: parse_porcelain_v1_z() is pure. GitStatusService methods are called from the UI
//                thread; background git invocations synchronize through an internal mutex.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <navigator/platform.hpp>
#include <navigator/types.hpp>

namespace navigator::git {

enum class GitFileState : std::uint8_t {
    None = 0,
    Ignored = 1,
    Untracked = 2,
    Added = 3,
    Modified = 4,
    Deleted = 5,
    Renamed = 6,
    Copied = 7,
    Conflict = 8,
};

// Short single-character badge label for UI display ("M", "A", "?", "D", "R", "C", "!", "").
[[nodiscard]] constexpr std::string_view git_state_badge(GitFileState state) noexcept {
    switch (state) {
    case GitFileState::Modified:
        return "M";
    case GitFileState::Added:
        return "A";
    case GitFileState::Untracked:
        return "?";
    case GitFileState::Deleted:
        return "D";
    case GitFileState::Renamed:
        return "R";
    case GitFileState::Copied:
        return "C";
    case GitFileState::Conflict:
        return "!";
    case GitFileState::Ignored:
        return "I";
    case GitFileState::None:
        return {};
    }
    return {};
}

// Higher severity wins when propagating child file states up to ancestor directories.
[[nodiscard]] constexpr GitFileState merge_dir_state(GitFileState a, GitFileState b) noexcept {
    return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}

struct GitRepoSnapshot {
    Path repo_root;
    std::string branch;
    // Keyed by repo-relative POSIX path (e.g. "src/app/main.cpp").
    std::unordered_map<std::string, GitFileState> file_states;
    // Keyed by repo-relative directory path (e.g. "src/app"); aggregated from file_states.
    std::unordered_map<std::string, GitFileState> dir_states;
    std::uint64_t generation = 0;
};

// Pure parser for `git status --porcelain=v1 -z -b --ignored=matching` output.
[[nodiscard]] GitRepoSnapshot parse_porcelain_v1_z(std::string_view stdout_bytes,
                                                   const Path& repo_root);

// Off-thread cached Git status service.
class GitStatusService {
public:
    explicit GitStatusService(std::shared_ptr<platform::IProcessRunner> runner = nullptr);
    ~GitStatusService();
    GitStatusService(const GitStatusService&) = delete;
    GitStatusService& operator=(const GitStatusService&) = delete;
    GitStatusService(GitStatusService&&) noexcept;
    GitStatusService& operator=(GitStatusService&&) noexcept;

    // Ensures status for the repository containing `dir` is fetched (or refreshed if stale).
    // Non-blocking: schedules work on a single background worker if not cached.
    void request_directory(const Path& dir);

    // Invalidates cached status for the repository containing `dir` so the next request re-runs
    // git status.
    void invalidate(const Path& dir);

    // Drains completed background snapshots into the UI-thread cache. Returns true if any cached
    // snapshot changed (so the UI knows to redraw).
    bool poll();

    // Instant UI-thread lookups against the cached snapshots.
    [[nodiscard]] GitFileState status_for(const Path& path) const;
    [[nodiscard]] std::optional<std::string> branch_for(const Path& dir) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace navigator::git
