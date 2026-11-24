// src/ui/browser/view_rows.cpp — virtualized grouping bucketing and flat display-list builder.
#include "browser/view_rows.hpp"

#include <array>
#include <chrono>
#include <ctime>

#include "browser/format.hpp"

namespace navigator::ui {
namespace {

constexpr std::uint8_t kMaxBuckets = 9;

std::int64_t to_unix_seconds(Timestamp ts) noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(ts.time_since_epoch()).count();
}

std::tm to_local_tm(Timestamp ts) noexcept {
    std::tm out{};
    const auto secs = static_cast<std::time_t>(to_unix_seconds(ts));
    localtime_r(&secs, &out);
    return out;
}

std::uint8_t date_bucket(Timestamp when, Timestamp now) noexcept {
    if (when == Timestamp{}) {
        return 6; // Unknown
    }
    const std::tm w = to_local_tm(when);
    const std::tm n = to_local_tm(now);
    if (w.tm_year == n.tm_year && w.tm_yday == n.tm_yday) {
        return 0; // Today
    }
    const std::int64_t now_s = to_unix_seconds(now);
    const std::int64_t when_s = to_unix_seconds(when);
    const std::int64_t diff_sec = now_s - when_s;
    if (diff_sec >= 0 && diff_sec < std::int64_t{2} * 86400) {
        // Check exact calendar yesterday.
        const auto yest_sec = static_cast<std::time_t>(now_s - 86400);
        std::tm y{};
        localtime_r(&yest_sec, &y);
        if (w.tm_year == y.tm_year && w.tm_yday == y.tm_yday) {
            return 1; // Yesterday
        }
    }
    if (diff_sec >= 0 && diff_sec < std::int64_t{7} * 86400) {
        return 2; // This week
    }
    if (diff_sec >= 0 && diff_sec < std::int64_t{30} * 86400) {
        return 3; // This month
    }
    if (w.tm_year == n.tm_year) {
        return 4; // Earlier this year
    }
    return 5; // Older
}

std::uint8_t type_bucket(const fs::DirEntry& entry) noexcept {
    if (entry.kind == fs::EntryKind::Directory ||
        (entry.kind == fs::EntryKind::Symlink && entry.target_kind == fs::EntryKind::Directory)) {
        return 0; // Folders
    }
    switch (file_tint(entry)) {
    case FileTint::Image: return 1;
    case FileTint::Video: return 2;
    case FileTint::Audio: return 3;
    case FileTint::Document:
    case FileTint::Pdf:
    case FileTint::Sheet:
    case FileTint::Slides: return 4;
    case FileTint::Archive: return 5;
    case FileTint::Code: return 6;
    case FileTint::Program: return 7;
    case FileTint::None: return 8;
    }
    return 8;
}

std::uint8_t size_bucket(const fs::DirEntry& entry) noexcept {
    if (entry.kind == fs::EntryKind::Directory ||
        (entry.kind == fs::EntryKind::Symlink && entry.target_kind == fs::EntryKind::Directory)) {
        return 0; // Folders
    }
    const std::uint64_t sz = entry.size;
    if (sz == 0) {
        return 1; // Empty
    }
    if (sz < 10'000ULL) {
        return 2; // Tiny (< 10 kB)
    }
    if (sz < 1'000'000ULL) {
        return 3; // Small (< 1 MB)
    }
    if (sz < 100'000'000ULL) {
        return 4; // Medium (< 100 MB)
    }
    if (sz < 1'000'000'000ULL) {
        return 5; // Large (< 1 GB)
    }
    return 6; // Huge (>= 1 GB)
}

} // namespace

std::uint8_t group_bucket(const fs::DirEntry& entry, view::GroupBy group_by,
                          Timestamp now) noexcept {
    switch (group_by) {
    case view::GroupBy::None: return 0;
    case view::GroupBy::Date: return date_bucket(entry.mtime, now);
    case view::GroupBy::Type: return type_bucket(entry);
    case view::GroupBy::Size: return size_bucket(entry);
    }
    return 0;
}

std::string_view group_bucket_key(view::GroupBy group_by, std::uint8_t bucket) noexcept {
    switch (group_by) {
    case view::GroupBy::None: return "";
    case view::GroupBy::Date:
        switch (bucket) {
        case 0: return "group.date.today";
        case 1: return "group.date.yesterday";
        case 2: return "group.date.this_week";
        case 3: return "group.date.this_month";
        case 4: return "group.date.this_year";
        case 5: return "group.date.older";
        default: return "group.date.unknown";
        }
    case view::GroupBy::Type:
        switch (bucket) {
        case 0: return "group.type.folders";
        case 1: return "group.type.images";
        case 2: return "group.type.videos";
        case 3: return "group.type.audio";
        case 4: return "group.type.documents";
        case 5: return "group.type.archives";
        case 6: return "group.type.code";
        case 7: return "group.type.programs";
        default: return "group.type.other";
        }
    case view::GroupBy::Size:
        switch (bucket) {
        case 0: return "group.size.folders";
        case 1: return "group.size.empty";
        case 2: return "group.size.tiny";
        case 3: return "group.size.small";
        case 4: return "group.size.medium";
        case 5: return "group.size.large";
        default: return "group.size.huge";
        }
    }
    return "";
}

GroupedRows build_grouped_rows(std::size_t row_count,
                               const fs::DirEntry* (*entry_at)(const void* ctx, std::size_t row),
                               const void* ctx, view::GroupBy group_by, Timestamp now,
                               const std::unordered_set<std::uint8_t>& collapsed) {
    GroupedRows out;
    if (row_count == 0 || entry_at == nullptr) {
        return out;
    }
    if (group_by == view::GroupBy::None) {
        out.items.reserve(row_count);
        for (std::size_t r = 0; r < row_count; ++r) {
            out.items.push_back(GroupDisplayItem{GroupDisplayItem::Kind::Entry, 0, r, 0, false});
        }
        return out;
    }

    std::array<std::vector<std::size_t>, kMaxBuckets> buckets;
    for (std::size_t r = 0; r < row_count; ++r) {
        const fs::DirEntry* e = entry_at(ctx, r);
        if (e == nullptr) {
            continue;
        }
        const std::uint8_t b = group_bucket(*e, group_by, now);
        if (b < kMaxBuckets) {
            buckets[b].push_back(r);
        }
    }

    out.items.reserve(row_count + kMaxBuckets);
    for (std::uint8_t b = 0; b < kMaxBuckets; ++b) {
        const auto& rows = buckets[b];
        if (rows.empty()) {
            continue;
        }
        ++out.group_count;
        const bool is_collapsed = collapsed.contains(b);
        out.items.push_back(GroupDisplayItem{GroupDisplayItem::Kind::Header, b, kInvalidRow,
                                             rows.size(), is_collapsed});
        if (!is_collapsed) {
            for (std::size_t r : rows) {
                out.items.push_back(
                    GroupDisplayItem{GroupDisplayItem::Kind::Entry, b, r, 0, false});
            }
        }
    }
    return out;
}

} // namespace navigator::ui
