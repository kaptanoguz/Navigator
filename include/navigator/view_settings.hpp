// navigator/view_settings.hpp
//
// Purpose: Phase 9A view modes, zoom axis, grouping, reorderable Details columns, and hierarchical
//          per-folder view settings (Global / Hierarchy / Folder lock, File Pilot 0.8.1 style),
//          plus JSON (de)serialization and atomic on-disk persistence (`view_settings.json`).
//          No SDL, no renderer, no JSON library types.
// Owner:   contracts (header); implementation in src/core (role: core-fs), tests in tests/core.
// Thread-safety: ViewSettings, ViewRule, ResolvedViewSettings and ViewSettingsStore are plain
//                values: distinct objects may be used from different threads, one object must not
//                be mutated concurrently. In the app the live ViewSettingsStore is owned by the
//                UI thread. JSON functions are pure CPU work; load_view_settings /
//                save_view_settings perform blocking file I/O (atomic temp-file + fsync + rename).
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::view {

// ---------------------------------------------------------------------------------------------
// View modes, grouping, columns and zoom
// ---------------------------------------------------------------------------------------------

enum class ViewMode : std::uint8_t {
    Details = 0, // virtualized table with reorderable columns
    Icons = 1,   // virtualized 2D icon/thumbnail grid with live zoom
    List = 2,    // virtualized multi-column flowing list
    Tree = 3,    // virtualized expandable directory tree
    Columns = 4, // Phase 10: Miller columns (horizontally scrolling hierarchy panes)
};

enum class GroupBy : std::uint8_t {
    None = 0,
    Date = 1, // Today, Yesterday, This Week, This Month, Earlier This Year, Older, Unknown
    Type = 2, // Folders, Images, Videos, Audio, Documents, Archives, Code, Other
    Size = 3, // Empty, Tiny, Small, Medium, Large, Huge
};

enum class ColumnId : std::uint8_t {
    Name = 0,
    Size = 1,
    Type = 2,
    Modified = 3,
};

inline constexpr std::size_t kColumnCount = 4;
using ColumnOrder = std::array<ColumnId, kColumnCount>;

inline constexpr ColumnOrder kDefaultColumnOrder{
    ColumnId::Name,
    ColumnId::Size,
    ColumnId::Type,
    ColumnId::Modified,
};

// Scope of a folder's active view settings (File Pilot 0.8.1 style lock).
enum class ViewScope : std::uint8_t {
    Global = 0,    // default settings shared by every unlocked folder
    Hierarchy = 1, // applies to rule_path and all its descendants unless overridden deeper
    Folder = 2,    // applies to rule_path alone
};

inline constexpr int kMinZoomPercent = 50;
inline constexpr int kMaxZoomPercent = 400;
inline constexpr int kDefaultZoomPercent = 100;
inline constexpr int kZoomStepPercent = 25;

[[nodiscard]] constexpr int clamp_zoom(int percent) noexcept {
    if (percent < kMinZoomPercent) {
        return kMinZoomPercent;
    }
    if (percent > kMaxZoomPercent) {
        return kMaxZoomPercent;
    }
    return percent;
}

// Normalizes a column order so every ColumnId appears exactly once (preserving the first
// occurrence of each valid column and appending any missing columns in kDefaultColumnOrder).
[[nodiscard]] constexpr ColumnOrder normalize_columns(const ColumnOrder& order) noexcept {
    ColumnOrder out{};
    std::array<bool, kColumnCount> seen{};
    std::size_t n = 0;
    for (ColumnId col : order) {
        const auto idx = static_cast<std::size_t>(col);
        if (idx < kColumnCount && !seen[idx]) {
            seen[idx] = true;
            out[n++] = col;
        }
    }
    for (ColumnId col : kDefaultColumnOrder) {
        const auto idx = static_cast<std::size_t>(col);
        if (!seen[idx]) {
            seen[idx] = true;
            out[n++] = col;
        }
    }
    return out;
}

// Moves the column at `from` to `to`, shifting the intervening columns. Out-of-range indices are
// clamped to [0, kColumnCount - 1]. Pure.
[[nodiscard]] constexpr ColumnOrder reorder_column(ColumnOrder order, std::size_t from,
                                                   std::size_t to) noexcept {
    order = normalize_columns(order);
    if (from >= kColumnCount) {
        from = kColumnCount - 1;
    }
    if (to >= kColumnCount) {
        to = kColumnCount - 1;
    }
    if (from == to) {
        return order;
    }
    const ColumnId moved = order[from];
    if (from < to) {
        for (std::size_t i = from; i < to; ++i) {
            order[i] = order[i + 1];
        }
    } else {
        for (std::size_t i = from; i > to; --i) {
            order[i] = order[i - 1];
        }
    }
    order[to] = moved;
    return order;
}

// Single zoom axis unifying Details <-> Icons:
//   - In Details, zooming in (delta_steps > 0) enters Icons mode at kMinZoomPercent +
//   (delta-1)*step;
//     zooming out stays in Details.
//   - In Icons, zooming out past kMinZoomPercent transitions back to Details (zoom reset to
//     kDefaultZoomPercent); otherwise steps within [kMinZoomPercent, kMaxZoomPercent].
//   - In List or Tree, zooming adjusts zoom_percent within [kMinZoomPercent, kMaxZoomPercent]
//     without changing mode.
[[nodiscard]] constexpr std::pair<ViewMode, int> step_zoom(ViewMode mode, int zoom_percent,
                                                           int delta_steps) noexcept {
    const int cur = clamp_zoom(zoom_percent);
    if (delta_steps == 0) {
        return {mode, cur};
    }
    if (mode == ViewMode::Details) {
        if (delta_steps > 0) {
            return {ViewMode::Icons,
                    clamp_zoom(kMinZoomPercent + ((delta_steps - 1) * kZoomStepPercent))};
        }
        return {ViewMode::Details, cur};
    }
    if (mode == ViewMode::Icons) {
        const int next = cur + (delta_steps * kZoomStepPercent);
        if (next < kMinZoomPercent) {
            return {ViewMode::Details, kDefaultZoomPercent};
        }
        return {ViewMode::Icons, clamp_zoom(next)};
    }
    return {mode, clamp_zoom(cur + (delta_steps * kZoomStepPercent))};
}

// Cycle order for commands.
[[nodiscard]] constexpr ViewMode next_view_mode(ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::Details: return ViewMode::Icons;
    case ViewMode::Icons: return ViewMode::List;
    case ViewMode::List: return ViewMode::Tree;
    case ViewMode::Tree: return ViewMode::Columns;
    case ViewMode::Columns: return ViewMode::Details;
    }
    return ViewMode::Details;
}

[[nodiscard]] constexpr GroupBy next_group_by(GroupBy group) noexcept {
    switch (group) {
    case GroupBy::None: return GroupBy::Date;
    case GroupBy::Date: return GroupBy::Type;
    case GroupBy::Type: return GroupBy::Size;
    case GroupBy::Size: return GroupBy::None;
    }
    return GroupBy::None;
}

[[nodiscard]] constexpr ViewScope next_view_scope(ViewScope scope) noexcept {
    switch (scope) {
    case ViewScope::Global: return ViewScope::Folder;
    case ViewScope::Folder: return ViewScope::Hierarchy;
    case ViewScope::Hierarchy: return ViewScope::Global;
    }
    return ViewScope::Global;
}

// String conversions (lowercase JSON / diagnostic names). Pure.
[[nodiscard]] std::string_view to_string(ViewMode mode) noexcept;
[[nodiscard]] std::optional<ViewMode> parse_view_mode(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(GroupBy group) noexcept;
[[nodiscard]] std::optional<GroupBy> parse_group_by(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(ColumnId col) noexcept;
[[nodiscard]] std::optional<ColumnId> parse_column_id(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(ViewScope scope) noexcept;
[[nodiscard]] std::optional<ViewScope> parse_view_scope(std::string_view name) noexcept;

// ---------------------------------------------------------------------------------------------
// ViewSettings value & hierarchical store
// ---------------------------------------------------------------------------------------------

struct ViewSettings {
    ViewMode mode = ViewMode::Details;
    int zoom_percent = kDefaultZoomPercent;
    GroupBy group_by = GroupBy::None;
    fs::SortOptions sort;
    bool show_hidden = false;
    ColumnOrder columns = kDefaultColumnOrder;

    friend bool operator==(const ViewSettings&, const ViewSettings&) = default;
};

[[nodiscard]] inline ViewSettings normalize(ViewSettings s) noexcept {
    s.zoom_percent = clamp_zoom(s.zoom_percent);
    s.columns = normalize_columns(s.columns);
    return s;
}

struct ViewRule {
    Path path;                           // canonical absolute path (no trailing '/' except "/")
    ViewScope scope = ViewScope::Folder; // Folder or Hierarchy (never Global)
    ViewSettings settings;

    friend bool operator==(const ViewRule&, const ViewRule&) = default;
};

struct ResolvedViewSettings {
    ViewSettings settings;
    ViewScope scope = ViewScope::Global;
    Path rule_path; // empty when scope == Global; otherwise the matching rule's path

    friend bool operator==(const ResolvedViewSettings&, const ResolvedViewSettings&) = default;
};

// Hierarchical view settings store:
//   Resolution for a folder `dir`:
//     1. Exact ViewScope::Folder rule for `dir`
//     2. Deepest ancestor of `dir` (including `dir` itself) with a ViewScope::Hierarchy rule
//     3. Global default
//   All paths are normalized (trailing slashes stripped except "/"). Relative or empty paths are
//   ignored by mutators and resolve to Global.
class ViewSettingsStore {
public:
    static constexpr int kVersion = 1;

    ViewSettingsStore() = default;

    [[nodiscard]] const ViewSettings& global() const noexcept { return global_; }
    void set_global(ViewSettings settings) noexcept { global_ = normalize(settings); }

    // Installs or replaces a Folder or Hierarchy rule on `dir`. Relative/empty paths are no-ops.
    void set_folder(const Path& dir, ViewSettings settings);
    void set_hierarchy(const Path& dir, ViewSettings settings);

    // Removes the Folder rule, Hierarchy rule, or both at `dir`. Returns true if anything changed.
    bool clear_folder(const Path& dir);
    bool clear_hierarchy(const Path& dir);
    bool clear_override(const Path& dir);

    // Resolves the effective settings for `dir`.
    [[nodiscard]] ResolvedViewSettings resolve(const Path& dir) const;

    // Writes `updated` into whichever rule governs `dir` (Folder -> that folder's rule;
    // Hierarchy -> rule_path's hierarchy rule; Global -> global default).
    void apply_change(const Path& dir, ViewSettings updated);

    // Sets the lock scope for `dir`, capturing `current` when creating a Folder/Hierarchy rule
    // and clearing any rule on `dir` when switching to Global.
    void set_lock(const Path& dir, ViewScope scope, ViewSettings current);

    // Cycles Global -> Folder -> Hierarchy -> Global for `dir`, returning the new scope.
    ViewScope cycle_lock(const Path& dir, ViewSettings current);

    // All explicit Folder and Hierarchy rules, sorted deterministically by (path, scope).
    [[nodiscard]] const std::vector<ViewRule>& rules() const noexcept { return rules_; }
    [[nodiscard]] bool empty() const noexcept {
        return rules_.empty() && global_ == ViewSettings{};
    }

    friend bool operator==(const ViewSettingsStore&, const ViewSettingsStore&) = default;

private:
    ViewSettings global_;
    std::vector<ViewRule> rules_; // kept sorted by (path.raw, scope)
};

// ---------------------------------------------------------------------------------------------
// JSON and files
// ---------------------------------------------------------------------------------------------
//
// Format (version 1):
//   { "version": 1,
//     "global": <view_settings>,
//     "rules": [
//       { "path": <bytes>, "scope": "folder" | "hierarchy", "settings": <view_settings> },
//       ...
//     ] }
// <view_settings> = {
//     "mode": "details" | "icons" | "list" | "tree",
//     "zoom": 100,
//     "group_by": "none" | "date" | "type" | "size",
//     "show_hidden": false,
//     "columns": [ "name", "size", "type", "modified" ],
//     "sort": { "key": ..., "descending": ..., "folders_first": ...,
//               "natural": ..., "case_insensitive": ... } }
// Loading is lenient: unknown fields ignored, invalid values fall back to defaults, invalid or
// relative rule paths are dropped.

inline constexpr std::string_view kViewSettingsFileName = "view_settings.json";

[[nodiscard]] inline Path view_settings_file(const Path& config_root) {
    return config_root / kViewSettingsFileName;
}

[[nodiscard]] Result<std::string> to_json(const ViewSettingsStore& store);
[[nodiscard]] Result<ViewSettingsStore> from_json(std::string_view text);

// Atomic file I/O (as layout.hpp load_session / save_session).
[[nodiscard]] Result<ViewSettingsStore> load_view_settings(const Path& file);
Result<void> save_view_settings(const Path& file, const ViewSettingsStore& store);

} // namespace navigator::view
