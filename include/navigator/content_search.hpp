// navigator/content_search.hpp
//
// Purpose: Phase 10 cancellable content search (ripgrep-like literal/case-insensitive search over
//          a directory tree). Skips binary files (NUL byte sniff), enforces a per-file size cap,
//          streams matches off-thread, and never blocks the UI thread.
// Owner:   contracts (header); implementation in src/search (role: search).
// Thread-safety: search_file_content() is reentrant. ContentSearchSession methods (start, cancel,
//                poll, active) are called from the UI thread; the worker thread communicates
//                through an internal mutex.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>

namespace navigator::search {

struct ContentSearchOptions {
    Path root;
    std::string query;
    bool case_sensitive = false;
    bool include_hidden = false;
    bool skip_binary = true;
    std::uint64_t max_file_bytes = std::uint64_t{4} * 1024U * 1024U; // 4 MiB default cap
    std::size_t max_matches = 2000;
    std::size_t max_matches_per_file = 20;
    std::size_t max_line_snippet = 200;
};

struct ContentMatch {
    Path file_path;
    std::string rel_path;          // relative to root
    std::uint32_t line_number = 0; // 1-based
    std::uint32_t column = 0;      // 0-based byte offset in line_snippet
    std::uint32_t match_length = 0;
    std::string line_snippet; // trimmed line text around match
};

struct ContentSearchStats {
    std::uint64_t files_scanned = 0;
    std::uint64_t files_skipped_binary = 0;
    std::uint64_t files_skipped_large = 0;
    std::uint64_t matches_found = 0;
    bool truncated = false; // hit max_matches
    bool cancelled = false;
    bool done = true;
};

struct ContentSearchPoll {
    std::vector<ContentMatch> new_matches;
    ContentSearchStats stats;
};

// Pure / synchronous helper: searches a single file, appending matches to `out`.
// Returns false if skipped (binary or exceeds max_file_bytes) or cancelled; true if scanned.
[[nodiscard]] Result<bool> search_file_content(const Path& file_path, std::string_view rel_path,
                                               const ContentSearchOptions& options,
                                               std::vector<ContentMatch>& out,
                                               std::stop_token stop = {});

// Off-thread streaming content search session owned by the UI.
class ContentSearchSession {
public:
    ContentSearchSession();
    ~ContentSearchSession();
    ContentSearchSession(const ContentSearchSession&) = delete;
    ContentSearchSession& operator=(const ContentSearchSession&) = delete;
    ContentSearchSession(ContentSearchSession&&) noexcept;
    ContentSearchSession& operator=(ContentSearchSession&&) noexcept;

    // Cancels any previous search and starts a new background search.
    // Empty query or empty root does nothing and leaves stats.done = true.
    void start(ContentSearchOptions options);

    // Requests cancellation of the running search (non-blocking).
    void cancel() noexcept;

    // Drains newly discovered matches and returns the latest stats (non-blocking, UI-thread safe).
    [[nodiscard]] ContentSearchPoll poll();

    [[nodiscard]] bool active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace navigator::search
