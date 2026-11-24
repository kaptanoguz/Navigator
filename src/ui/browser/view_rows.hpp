// src/ui/browser/view_rows.hpp
//
// Phase 9A virtualized grouping: turns a BrowserModel's visible rows into a flat list of
// collapsible group headers and entry rows for Details, Icons, and List views.
// Pure CPU (no SDL, no I/O); O(N) build; thread-safe for distinct objects.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>
#include <navigator/view_settings.hpp>

namespace navigator::ui {

inline constexpr std::size_t kInvalidRow = static_cast<std::size_t>(-1);

struct GroupDisplayItem {
    enum class Kind : std::uint8_t {
        Header = 0,
        Entry = 1,
    };
    Kind kind = Kind::Entry;
    std::uint8_t bucket = 0;
    std::size_t row = kInvalidRow; // Entry: row index in BrowserModel; Header: kInvalidRow
    std::size_t count = 0;         // Header: number of entries in this group
    bool collapsed = false;        // Header: whether the group is currently collapsed
};

// Bucket id for `entry` under `group_by` relative to `now`.
// Stable small integers in display order (0 = first group).
[[nodiscard]] std::uint8_t group_bucket(const fs::DirEntry& entry, view::GroupBy group_by,
                                        Timestamp now) noexcept;

// i18n catalog key for a (GroupBy, bucket) pair (e.g. "group.date.today").
[[nodiscard]] std::string_view group_bucket_key(view::GroupBy group_by,
                                                std::uint8_t bucket) noexcept;

// Builds the flat virtualized display list for `row_count` rows fetched via `entry_at(row)`.
// When `group_by == GroupBy::None`, returns one Entry item per row with no headers.
// Preserves the relative order of rows inside each bucket.
struct GroupedRows {
    std::vector<GroupDisplayItem> items;
    std::size_t group_count = 0;
};

[[nodiscard]] GroupedRows build_grouped_rows(std::size_t row_count,
                                             const fs::DirEntry* (*entry_at)(const void* ctx,
                                                                             std::size_t row),
                                             const void* ctx, view::GroupBy group_by, Timestamp now,
                                             const std::unordered_set<std::uint8_t>& collapsed);

} // namespace navigator::ui
