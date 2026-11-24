// src/core/view_settings.cpp — Phase 9A view settings store and string helpers.
#include <navigator/view_settings.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace navigator::view {
namespace {

// Canonicalizes an absolute path by stripping redundant trailing slashes (except "/").
// Returns std::nullopt for empty or relative paths.
std::optional<Path> canonical_dir(const Path& dir) {
    const std::string& raw = dir.native();
    if (raw.empty() || raw[0] != '/') {
        return std::nullopt;
    }
    std::string s = raw;
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return Path{std::move(s)};
}

// True when `ancestor` is `dir` itself or a directory prefix of `dir`.
bool is_ancestor_or_same(std::string_view ancestor, std::string_view dir) noexcept {
    if (ancestor == "/") {
        return !dir.empty() && dir[0] == '/';
    }
    if (dir.size() < ancestor.size()) {
        return false;
    }
    if (dir.compare(0, ancestor.size(), ancestor) != 0) {
        return false;
    }
    return dir.size() == ancestor.size() || dir[ancestor.size()] == '/';
}

bool rule_less(const ViewRule& a, const ViewRule& b) noexcept {
    if (a.path.native() != b.path.native()) {
        return a.path.native() < b.path.native();
    }
    return static_cast<std::uint8_t>(a.scope) < static_cast<std::uint8_t>(b.scope);
}

} // namespace

std::string_view to_string(ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::Details: return "details";
    case ViewMode::Icons: return "icons";
    case ViewMode::List: return "list";
    case ViewMode::Tree: return "tree";
    }
    return "details";
}

std::optional<ViewMode> parse_view_mode(std::string_view name) noexcept {
    if (name == "details") {
        return ViewMode::Details;
    }
    if (name == "icons") {
        return ViewMode::Icons;
    }
    if (name == "list") {
        return ViewMode::List;
    }
    if (name == "tree") {
        return ViewMode::Tree;
    }
    return std::nullopt;
}

std::string_view to_string(GroupBy group) noexcept {
    switch (group) {
    case GroupBy::None: return "none";
    case GroupBy::Date: return "date";
    case GroupBy::Type: return "type";
    case GroupBy::Size: return "size";
    }
    return "none";
}

std::optional<GroupBy> parse_group_by(std::string_view name) noexcept {
    if (name == "none") {
        return GroupBy::None;
    }
    if (name == "date") {
        return GroupBy::Date;
    }
    if (name == "type") {
        return GroupBy::Type;
    }
    if (name == "size") {
        return GroupBy::Size;
    }
    return std::nullopt;
}

std::string_view to_string(ColumnId col) noexcept {
    switch (col) {
    case ColumnId::Name: return "name";
    case ColumnId::Size: return "size";
    case ColumnId::Type: return "type";
    case ColumnId::Modified: return "modified";
    }
    return "name";
}

std::optional<ColumnId> parse_column_id(std::string_view name) noexcept {
    if (name == "name") {
        return ColumnId::Name;
    }
    if (name == "size") {
        return ColumnId::Size;
    }
    if (name == "type") {
        return ColumnId::Type;
    }
    if (name == "modified") {
        return ColumnId::Modified;
    }
    return std::nullopt;
}

std::string_view to_string(ViewScope scope) noexcept {
    switch (scope) {
    case ViewScope::Global: return "global";
    case ViewScope::Hierarchy: return "hierarchy";
    case ViewScope::Folder: return "folder";
    }
    return "global";
}

std::optional<ViewScope> parse_view_scope(std::string_view name) noexcept {
    if (name == "global") {
        return ViewScope::Global;
    }
    if (name == "hierarchy") {
        return ViewScope::Hierarchy;
    }
    if (name == "folder") {
        return ViewScope::Folder;
    }
    return std::nullopt;
}

void ViewSettingsStore::set_folder(const Path& dir, ViewSettings settings) {
    const auto canon = canonical_dir(dir);
    if (!canon) {
        return;
    }
    ViewRule rule{*canon, ViewScope::Folder, normalize(settings)};
    auto it = std::lower_bound(rules_.begin(), rules_.end(), rule, rule_less);
    if (it != rules_.end() && it->path == rule.path && it->scope == rule.scope) {
        it->settings = rule.settings;
    } else {
        rules_.insert(it, std::move(rule));
    }
}

void ViewSettingsStore::set_hierarchy(const Path& dir, ViewSettings settings) {
    const auto canon = canonical_dir(dir);
    if (!canon) {
        return;
    }
    ViewRule rule{*canon, ViewScope::Hierarchy, normalize(settings)};
    auto it = std::lower_bound(rules_.begin(), rules_.end(), rule, rule_less);
    if (it != rules_.end() && it->path == rule.path && it->scope == rule.scope) {
        it->settings = rule.settings;
    } else {
        rules_.insert(it, std::move(rule));
    }
}

bool ViewSettingsStore::clear_folder(const Path& dir) {
    const auto canon = canonical_dir(dir);
    if (!canon) {
        return false;
    }
    const auto before = rules_.size();
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
                                [&](const ViewRule& r) {
                                    return r.scope == ViewScope::Folder && r.path == *canon;
                                }),
                 rules_.end());
    return rules_.size() != before;
}

bool ViewSettingsStore::clear_hierarchy(const Path& dir) {
    const auto canon = canonical_dir(dir);
    if (!canon) {
        return false;
    }
    const auto before = rules_.size();
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
                                [&](const ViewRule& r) {
                                    return r.scope == ViewScope::Hierarchy && r.path == *canon;
                                }),
                 rules_.end());
    return rules_.size() != before;
}

bool ViewSettingsStore::clear_override(const Path& dir) {
    const auto canon = canonical_dir(dir);
    if (!canon) {
        return false;
    }
    const auto before = rules_.size();
    rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
                                [&](const ViewRule& r) { return r.path == *canon; }),
                 rules_.end());
    return rules_.size() != before;
}

ResolvedViewSettings ViewSettingsStore::resolve(const Path& dir) const {
    const auto canon = canonical_dir(dir);
    if (!canon) {
        return ResolvedViewSettings{global_, ViewScope::Global, Path{}};
    }
    // 1. Exact Folder rule.
    for (const ViewRule& r : rules_) {
        if (r.scope == ViewScope::Folder && r.path == *canon) {
            return ResolvedViewSettings{r.settings, ViewScope::Folder, r.path};
        }
    }
    // 2. Deepest ancestor (including `dir` itself) with a Hierarchy rule.
    const ViewRule* best = nullptr;
    for (const ViewRule& r : rules_) {
        if (r.scope != ViewScope::Hierarchy) {
            continue;
        }
        if (is_ancestor_or_same(r.path.native(), canon->native())) {
            if (best == nullptr || r.path.native().size() > best->path.native().size()) {
                best = &r;
            }
        }
    }
    if (best != nullptr) {
        return ResolvedViewSettings{best->settings, ViewScope::Hierarchy, best->path};
    }
    // 3. Global default.
    return ResolvedViewSettings{global_, ViewScope::Global, Path{}};
}

void ViewSettingsStore::apply_change(const Path& dir, ViewSettings updated) {
    updated = normalize(updated);
    const ResolvedViewSettings resolved = resolve(dir);
    switch (resolved.scope) {
    case ViewScope::Folder: set_folder(resolved.rule_path, updated); break;
    case ViewScope::Hierarchy: set_hierarchy(resolved.rule_path, updated); break;
    case ViewScope::Global: set_global(updated); break;
    }
}

void ViewSettingsStore::set_lock(const Path& dir, ViewScope scope, ViewSettings current) {
    const auto canon = canonical_dir(dir);
    if (!canon) {
        return;
    }
    current = normalize(current);
    switch (scope) {
    case ViewScope::Global:
        clear_override(*canon);
        break;
    case ViewScope::Folder:
        clear_hierarchy(*canon);
        set_folder(*canon, current);
        break;
    case ViewScope::Hierarchy:
        clear_folder(*canon);
        set_hierarchy(*canon, current);
        break;
    }
}

ViewScope ViewSettingsStore::cycle_lock(const Path& dir, ViewSettings current) {
    const auto canon = canonical_dir(dir);
    if (!canon) {
        return ViewScope::Global;
    }
    const ResolvedViewSettings resolved = resolve(*canon);
    ViewScope next = ViewScope::Folder;
    if (resolved.scope == ViewScope::Folder && resolved.rule_path == *canon) {
        next = ViewScope::Hierarchy;
    } else if (resolved.scope == ViewScope::Hierarchy && resolved.rule_path == *canon) {
        next = ViewScope::Global;
    } else {
        // Global or inherited from an ancestor hierarchy -> pin this specific folder first.
        next = ViewScope::Folder;
    }
    set_lock(*canon, next, current);
    return next;
}

} // namespace navigator::view
