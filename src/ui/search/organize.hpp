// src/ui/search/organize.hpp
//
// Phase 5 (docs/architecture.md "Phase 5"): the data of a flattened view and the pure work that
// turns it into display rows.
//
//   - FlatChunk: one walk batch (or one index result), immutable once published, shared between
//     the UI thread and the search worker through shared_ptr. Entries get dense ids in arrival
//     order (chunk.first_entry + i); directories keep the walk's table indices.
//   - FlatTables: dense id -> entry and directory index -> relative path lookups over a chunk list
//     (pointers into the chunks; O(1)).
//   - organize(): filters (type filter, then query) and orders the entries of a flattened view,
//     incrementally: a state that already holds the hits of earlier chunks only matches the new
//     chunks and merges them in (O(n) per call), so a 300k-entry walk never re-sorts from scratch.
//     Order: ranks_before() when the query is not empty (relevance), otherwise the tab's sort
//     options with the relative parent path as the last tie-breaker (docs/architecture.md
//     "View model per tab"). Type counts for the types popup cover every entry (query and type
//     filter ignored), in summarize_types() order.
//   - filter_listing_rows(): the folder filter over a listing, optionally only over the hits of
//     the previous query (search::narrows).
// Thread-safety: pure functions and plain values; an OrganizeState is used by one thread at a
// time (the search worker).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/search.hpp>
#include <navigator/types.hpp>
#include <navigator/walk.hpp>

namespace navigator::ui {

struct FlatChunk {
    std::uint32_t first_entry = 0;     // dense id of entries[0]
    std::uint32_t first_dir = 0;       // directory table index of new_dirs[0]
    std::vector<std::string> new_dirs; // relative paths (walk.hpp)
    std::vector<fs::WalkEntry> entries;
};
using FlatChunkPtr = std::shared_ptr<const FlatChunk>;

class FlatTables {
public:
    // Chunks must arrive in order (first_entry == size(), first_dir == dir_count()).
    void append(const FlatChunkPtr& chunk);
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t dir_count() const noexcept { return dirs_.size(); }
    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }
    [[nodiscard]] const fs::WalkEntry& entry(std::uint32_t id) const noexcept {
        return *entries_[id];
    }
    // Relative path of a directory index ("" for the root and unknown indices).
    [[nodiscard]] std::string_view dir(std::uint32_t index) const noexcept;
    // Relative parent path of an entry ("" for children of the root).
    [[nodiscard]] std::string_view parent_of(std::uint32_t id) const noexcept {
        return dir(entries_[id]->dir);
    }
    [[nodiscard]] const std::vector<FlatChunkPtr>& chunks() const noexcept { return chunks_; }

private:
    std::vector<FlatChunkPtr> chunks_;
    std::vector<const fs::WalkEntry*> entries_;
    std::vector<const std::string*> dirs_{nullptr}; // index 0: the root ("")
};

struct OrganizeConfig {
    search::Query query;
    search::TypeFilter types;
    fs::SortOptions sort;
    // Relevance order (ranks_before). The ui sets it when the query is not empty and the user
    // has not clicked a column header since the query changed.
    bool rank = false;
};

struct FlatHit {
    std::uint32_t id = 0;
    std::int32_t score = 0;
};

// Worker-side state carried across incremental organize() calls for one OrganizeConfig.
struct OrganizeState {
    FlatTables tables;
    std::vector<FlatHit> hits; // ordered
    std::map<search::TypeKey, std::size_t> counts;
    search::Matcher matcher;
    bool configured = false;
};

struct OrganizeOutput {
    std::vector<std::uint32_t> order; // ids of the hits, display order
    std::size_t processed = 0;        // entries considered (ids < processed)
    std::vector<search::TypeCount> types;
};

// Appends `chunks` to `state` (which must have seen every earlier chunk of this view), matches
// them and merges the hits. Errors: Cancelled (`state` must then be discarded), OutOfMemory.
[[nodiscard]] Result<OrganizeOutput> organize(OrganizeState& state, const OrganizeConfig& config,
                                              Span<const FlatChunkPtr> chunks,
                                              const std::stop_token& stop = {});

// Display order of two flattened entries in sort mode (tab SortOptions; ties: names, raw bytes,
// then the relative parent path). Exposed for tests.
[[nodiscard]] bool flat_sort_before(const fs::WalkEntry& a, std::string_view parent_a,
                                    const fs::WalkEntry& b, std::string_view parent_b,
                                    const fs::SortOptions& sort) noexcept;

// Types popup rows from accumulated counts, in summarize_types() order; `visible` from `filter`.
// Hidden types of `filter` that are not in `counts` are appended with count 0 when
// `include_absent_hidden` (index results only show allowed types, so the popup must still offer
// the hidden ones back).
[[nodiscard]] std::vector<search::TypeCount>
type_rows(const std::map<search::TypeKey, std::size_t>& counts, const search::TypeFilter& filter,
          bool include_absent_hidden = false);

// Folder filter over a listing. `subset` (sorted listing indices, the previous hits) restricts
// the candidates when the new query narrows the previous one. `out` receives listing indices
// in listing order. Errors: Cancelled, OutOfMemory.
[[nodiscard]] Result<void>
filter_listing_rows(Span<const fs::DirEntry> entries, search::Matcher& matcher,
                    const search::TypeFilter& types, const std::vector<std::uint32_t>* subset,
                    std::vector<std::uint32_t>& out, const std::stop_token& stop = {});

} // namespace navigator::ui
