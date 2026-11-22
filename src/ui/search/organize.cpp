// src/ui/search/organize.cpp — see organize.hpp.
#include "search/organize.hpp"

#include <algorithm>
#include <compare>
#include <new>
#include <utility>

namespace navigator::ui {
namespace {

constexpr std::size_t kStopInterval = search::kFilterStopCheckInterval;

std::weak_ordering name_order(std::string_view a, std::string_view b,
                              const fs::SortOptions& sort) noexcept {
    std::weak_ordering c = fs::compare_names(a, b, sort);
    if (c == 0) {
        c = a <=> b;
    }
    return c;
}

std::weak_ordering parent_order(std::string_view a, std::string_view b) noexcept {
    const fs::SortOptions defaults;
    return name_order(a, b, defaults);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// FlatTables
// ---------------------------------------------------------------------------------------------

void FlatTables::append(const FlatChunkPtr& chunk) {
    if (!chunk) {
        return;
    }
    chunks_.push_back(chunk);
    // Directory indices are the walk's: new_dirs[k] has index first_dir + k. Gaps (never
    // expected) are filled with the root so lookups stay in bounds.
    const std::size_t need = static_cast<std::size_t>(chunk->first_dir) + chunk->new_dirs.size();
    if (dirs_.size() < need) {
        dirs_.resize(need, nullptr);
    }
    for (std::size_t k = 0; k < chunk->new_dirs.size(); ++k) {
        dirs_[chunk->first_dir + k] = &chunk->new_dirs[k];
    }
    // No reserve(size + n): that reallocates on every append, quadratic over a 1M-entry walk.
    for (const fs::WalkEntry& e : chunk->entries) {
        entries_.push_back(&e);
    }
}

void FlatTables::clear() noexcept {
    chunks_.clear();
    entries_.clear();
    dirs_.assign(1, nullptr);
}

std::string_view FlatTables::dir(std::uint32_t index) const noexcept {
    if (index >= dirs_.size() || dirs_[index] == nullptr) {
        return {};
    }
    return *dirs_[index];
}

// ---------------------------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------------------------

bool flat_sort_before(const fs::WalkEntry& a, std::string_view parent_a, const fs::WalkEntry& b,
                      std::string_view parent_b, const fs::SortOptions& sort) noexcept {
    const fs::DirEntry& x = a.entry;
    const fs::DirEntry& y = b.entry;
    if (sort.folders_first) {
        const bool dx = fs::is_directory_like(x);
        const bool dy = fs::is_directory_like(y);
        if (dx != dy) {
            return dx; // folders stay first in both directions, as in the listing
        }
    }
    std::weak_ordering c = std::weak_ordering::equivalent;
    switch (sort.key) {
    case fs::SortKey::Size: c = x.size <=> y.size; break;
    case fs::SortKey::Modified: c = x.mtime <=> y.mtime; break;
    case fs::SortKey::Extension:
        c = fs::compare_names(fs::extension(x.name), fs::extension(y.name), sort);
        break;
    case fs::SortKey::Name:
    case fs::SortKey::Count: break;
    }
    if (c == 0) {
        c = name_order(x.name, y.name, sort);
    }
    if (c == 0) {
        c = parent_order(parent_a, parent_b);
    }
    return sort.descending ? c > 0 : c < 0;
}

namespace {

struct HitLess {
    const FlatTables* tables = nullptr;
    const OrganizeConfig* config = nullptr;

    bool operator()(const FlatHit& a, const FlatHit& b) const noexcept {
        const fs::WalkEntry& ea = tables->entry(a.id);
        const fs::WalkEntry& eb = tables->entry(b.id);
        if (config->rank) {
            const std::string_view pa = ea.depth <= 1 ? std::string_view{} : tables->dir(ea.dir);
            const std::string_view pb = eb.depth <= 1 ? std::string_view{} : tables->dir(eb.dir);
            return search::ranks_before(search::RankInfo{a.score, ea.depth, ea.entry.name, pa},
                                        search::RankInfo{b.score, eb.depth, eb.entry.name, pb});
        }
        return flat_sort_before(ea, tables->dir(ea.dir), eb, tables->dir(eb.dir), config->sort);
    }
};

} // namespace

std::vector<search::TypeCount> type_rows(const std::map<search::TypeKey, std::size_t>& counts,
                                         const search::TypeFilter& filter,
                                         bool include_absent_hidden) {
    std::vector<search::TypeCount> out;
    out.reserve(counts.size() + 2);
    std::vector<search::TypeCount> exts;
    const auto add = [&](const search::TypeKey& key, std::size_t n) {
        search::TypeCount row;
        row.key = key;
        row.count = n;
        row.visible = search::is_visible(filter, key);
        if (key.kind == search::TypeKind::Extension) {
            exts.push_back(std::move(row));
        } else {
            out.push_back(std::move(row));
        }
    };
    const search::TypeKey folder{search::TypeKind::Folder, {}};
    const search::TypeKey none{search::TypeKind::NoExtension, {}};
    if (const auto it = counts.find(folder); it != counts.end() && it->second > 0) {
        add(folder, it->second);
    } else if (include_absent_hidden && filter.hide_folders) {
        add(folder, 0);
    }
    if (const auto it = counts.find(none); it != counts.end() && it->second > 0) {
        add(none, it->second);
    } else if (include_absent_hidden && filter.hide_no_extension) {
        add(none, 0);
    }
    for (const auto& [key, n] : counts) {
        if (key.kind == search::TypeKind::Extension && n > 0) {
            add(key, n);
        }
    }
    if (include_absent_hidden) {
        for (const std::string& ext : filter.hidden_extensions) {
            const search::TypeKey key{search::TypeKind::Extension, ext};
            if (const auto it = counts.find(key); it == counts.end() || it->second == 0) {
                add(key, 0);
            }
        }
    }
    std::sort(exts.begin(), exts.end(), [](const search::TypeCount& a, const search::TypeCount& b) {
        if (a.count != b.count) {
            return a.count > b.count;
        }
        return a.key.extension < b.key.extension;
    });
    for (search::TypeCount& e : exts) {
        out.push_back(std::move(e));
    }
    return out;
}

Result<OrganizeOutput> organize(OrganizeState& state, const OrganizeConfig& config,
                                Span<const FlatChunkPtr> chunks, const std::stop_token& stop) {
    try {
        if (!state.configured) {
            state.matcher = search::Matcher(config.query);
            state.configured = true;
        }
        const std::size_t old_size = state.hits.size();
        const std::size_t first_new = state.tables.size();
        for (const FlatChunkPtr& c : chunks) {
            state.tables.append(c);
        }
        const std::size_t total = state.tables.size();
        const bool empty_query = config.query.empty();
        for (std::size_t i = first_new; i < total; ++i) {
            if ((i - first_new) % kStopInterval == kStopInterval - 1 && stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "organize cancelled");
            }
            const auto id = static_cast<std::uint32_t>(i);
            const fs::DirEntry& e = state.tables.entry(id).entry;
            ++state.counts[search::type_key_of(e)];
            if (!search::allows(config.types, e)) {
                continue;
            }
            std::int32_t score = 0;
            if (!empty_query) {
                const std::optional<std::int32_t> s = state.matcher.score(e.name);
                if (!s) {
                    continue;
                }
                score = *s;
            }
            state.hits.push_back(FlatHit{id, score});
        }
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "organize cancelled");
        }
        const HitLess less{&state.tables, &config};
        const auto mid = state.hits.begin() + static_cast<std::ptrdiff_t>(old_size);
        std::sort(mid, state.hits.end(), less);
        std::inplace_merge(state.hits.begin(), mid, state.hits.end(), less);

        OrganizeOutput out;
        out.processed = total;
        out.order.reserve(state.hits.size());
        for (const FlatHit& h : state.hits) {
            out.order.push_back(h.id);
        }
        out.types = type_rows(state.counts, config.types);
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "organize: out of memory");
    }
}

Result<void> filter_listing_rows(Span<const fs::DirEntry> entries, search::Matcher& matcher,
                                 const search::TypeFilter& types,
                                 const std::vector<std::uint32_t>* subset,
                                 std::vector<std::uint32_t>& out, const std::stop_token& stop) {
    out.clear();
    try {
        if (subset == nullptr) {
            std::vector<search::FilterHit> hits;
            if (Result<void> r = search::filter_entries(entries, matcher, types, hits, stop); !r) {
                return r;
            }
            out.reserve(hits.size());
            for (const search::FilterHit& h : hits) {
                out.push_back(h.index);
            }
            return ok();
        }
        out.reserve(subset->size());
        std::size_t n = 0;
        for (const std::uint32_t i : *subset) {
            if (++n % kStopInterval == 0 && stop.stop_requested()) {
                out.clear();
                return fail(ErrorCode::Cancelled, "filter cancelled");
            }
            if (i >= entries.size()) {
                continue;
            }
            const fs::DirEntry& e = entries[i];
            if (search::allows(types, e) && matcher.matches(e.name)) {
                out.push_back(i);
            }
        }
        return ok();
    } catch (const std::bad_alloc&) {
        out.clear();
        return fail(ErrorCode::OutOfMemory, "filter: out of memory");
    }
}

} // namespace navigator::ui
