// src/search/types.cpp: type visibility filter, types popup summary and the filter pipeline
// (navigator/search.hpp "Type visibility filter" and "Filter pipeline").
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/search.hpp>
#include <navigator/walk.hpp>

#include "match_core.hpp"

namespace navigator::search {

TypeKey type_key_of(const fs::DirEntry& entry) {
    if (fs::is_directory_like(entry)) {
        return TypeKey{TypeKind::Folder, {}};
    }
    const std::string_view ext = fs::extension(entry.name);
    if (ext.empty()) {
        return TypeKey{TypeKind::NoExtension, {}};
    }
    return TypeKey{TypeKind::Extension, fs::fold_name(ext)};
}

bool is_visible(const TypeFilter& filter, const TypeKey& key) noexcept {
    switch (key.kind) {
    case TypeKind::Folder: return !filter.hide_folders;
    case TypeKind::NoExtension: return !filter.hide_no_extension;
    case TypeKind::Extension: break;
    }
    return !std::binary_search(filter.hidden_extensions.begin(), filter.hidden_extensions.end(),
                               key.extension);
}

bool allows(const TypeFilter& filter, const fs::DirEntry& entry) {
    std::string buffer;
    return detail::allows_name(filter, fs::is_directory_like(entry), entry.name, buffer);
}

void set_visible(TypeFilter& filter, const TypeKey& key, bool visible) {
    switch (key.kind) {
    case TypeKind::Folder: filter.hide_folders = !visible; return;
    case TypeKind::NoExtension: filter.hide_no_extension = !visible; return;
    case TypeKind::Extension: break;
    }
    std::vector<std::string>& hidden = filter.hidden_extensions;
    const auto it = std::lower_bound(hidden.begin(), hidden.end(), key.extension);
    const bool present = it != hidden.end() && *it == key.extension;
    if (visible && present) {
        hidden.erase(it);
    } else if (!visible && !present && !key.extension.empty() &&
               key.extension.find_first_of(std::string_view("/\0", 2)) == std::string::npos) {
        hidden.insert(it, key.extension);
    }
}

void toggle_all(TypeFilter& filter, Span<const TypeCount> present) {
    const bool all_visible = std::all_of(present.begin(), present.end(), [&](const TypeCount& t) {
        return is_visible(filter, t.key);
    });
    for (const TypeCount& t : present) {
        set_visible(filter, t.key, !all_visible);
    }
}

void normalize(TypeFilter& filter) {
    std::vector<std::string> out;
    out.reserve(filter.hidden_extensions.size());
    for (const std::string& ext : filter.hidden_extensions) {
        if (ext.empty() || ext.find_first_of(std::string_view("/\0", 2)) != std::string::npos) {
            continue;
        }
        out.push_back(fs::fold_name(ext));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    filter.hidden_extensions = std::move(out);
}

std::vector<TypeCount> summarize_types(Span<const fs::DirEntry> entries, bool show_hidden,
                                       const TypeFilter& filter) {
    std::size_t folders = 0;
    std::size_t no_ext = 0;
    std::map<std::string, std::size_t, std::less<>> exts;
    std::string buffer;
    for (const fs::DirEntry& e : entries) {
        if (!show_hidden && e.hidden) {
            continue;
        }
        if (fs::is_directory_like(e)) {
            ++folders;
            continue;
        }
        const std::string_view ext = fs::extension(e.name);
        if (ext.empty()) {
            ++no_ext;
            continue;
        }
        fs::fold_name_into(ext, buffer);
        const auto it = exts.find(buffer);
        if (it != exts.end()) {
            ++it->second;
        } else {
            exts.emplace(buffer, 1);
        }
    }
    std::vector<TypeCount> out;
    out.reserve(exts.size() + 2);
    const auto push = [&](TypeKey key, std::size_t count) {
        const bool visible = is_visible(filter, key);
        out.push_back(TypeCount{std::move(key), count, visible});
    };
    if (folders > 0) {
        push(TypeKey{TypeKind::Folder, {}}, folders);
    }
    if (no_ext > 0) {
        push(TypeKey{TypeKind::NoExtension, {}}, no_ext);
    }
    const std::size_t first_ext = out.size();
    for (const auto& [ext, count] : exts) {
        push(TypeKey{TypeKind::Extension, ext}, count);
    }
    // exts is ordered by bytes; a stable sort by descending count keeps that as the tie-break.
    std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first_ext), out.end(),
                     [](const TypeCount& a, const TypeCount& b) { return a.count > b.count; });
    return out;
}

namespace {

template <class Entry, class Get>
Result<void> filter_impl(Span<const Entry> entries, Matcher& matcher, const TypeFilter& types,
                         std::vector<FilterHit>& out, const std::stop_token& stop, Get get) {
    try {
        out.clear();
        std::string buffer;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i % kFilterStopCheckInterval == 0 && stop.stop_requested()) {
                out.clear();
                return fail(ErrorCode::Cancelled, "filter cancelled");
            }
            const fs::DirEntry& e = get(entries[i]);
            if (!detail::allows_name(types, fs::is_directory_like(e), e.name, buffer)) {
                continue;
            }
            const std::optional<std::int32_t> score = matcher.score(e.name);
            if (score) {
                out.push_back(FilterHit{static_cast<std::uint32_t>(i), *score});
            }
        }
        return ok();
    } catch (const std::bad_alloc&) {
        out.clear();
        return fail(ErrorCode::OutOfMemory, "filter: out of memory");
    }
}

} // namespace

// The by-value std::stop_token parameters are fixed by search.hpp.
// NOLINTBEGIN(performance-unnecessary-value-param)
Result<void> filter_entries(Span<const fs::DirEntry> entries, Matcher& matcher,
                            const TypeFilter& types, std::vector<FilterHit>& out,
                            std::stop_token stop) {
    return filter_impl(entries, matcher, types, out, stop,
                       [](const fs::DirEntry& e) -> const fs::DirEntry& { return e; });
}

Result<void> filter_entries(Span<const fs::WalkEntry> entries, Matcher& matcher,
                            const TypeFilter& types, std::vector<FilterHit>& out,
                            std::stop_token stop) {
    return filter_impl(entries, matcher, types, out, stop,
                       [](const fs::WalkEntry& e) -> const fs::DirEntry& { return e.entry; });
}
// NOLINTEND(performance-unnecessary-value-param)

} // namespace navigator::search
