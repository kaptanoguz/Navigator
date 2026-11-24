// Pure context menu model: builders, shortcut application, pin/destructive arrangement (M1-M3),
// and searchable filter (M4-M5) (navigator/menu.hpp).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/menu.hpp>

namespace navigator::menu {
namespace {

constexpr std::int32_t kMenuCommandNameDiscount = 24;

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

Item make_separator() {
    Item sep;
    sep.kind = Item::Kind::Separator;
    sep.pinnable = false;
    return sep;
}

Item make_submenu(std::string id, std::string label_key) {
    Item sub;
    sub.kind = Item::Kind::Submenu;
    sub.id = std::move(id);
    sub.label_key = std::move(label_key);
    sub.pinnable = false;
    return sub;
}

std::optional<Item> make_cmd(const CommandRegistry& registry, const CommandContext& context,
                             CommandId id, std::uint64_t arg = 0,
                             std::string_view label_override = {}, bool require_applicable = false,
                             bool force_variant_id = false) {
    const CommandInfo* info = registry.find(id);
    if (info == nullptr) {
        return std::nullopt;
    }
    if (require_applicable && !is_applicable(*info, context)) {
        return std::nullopt;
    }
    Item item;
    item.kind = Item::Kind::Command;
    item.command = Command{id, arg};
    item.id = std::string(info->name);
    if (force_variant_id || (info->arg_kind == ArgKind::Flags && arg != 0)) {
        item.id.append("#").append(std::to_string(arg));
    }
    item.label_key =
        label_override.empty() ? std::string(info->title_key) : std::string(label_override);
    item.danger = effective_danger(*info, context);
    item.enabled = true;
    item.pinnable = (item.danger != Danger::Destructive);
    return item;
}

void push_if(std::vector<Item>& out, std::optional<Item> item) {
    if (item) {
        out.push_back(std::move(*item));
    }
}

void push_sep_if_needed(std::vector<Item>& out) {
    if (!out.empty() && out.back().kind != Item::Kind::Separator) {
        out.push_back(make_separator());
    }
}

void trim_separators(std::vector<Item>& items) {
    std::vector<Item> clean;
    clean.reserve(items.size());
    for (Item& item : items) {
        if (item.kind == Item::Kind::Separator) {
            if (clean.empty() || clean.back().kind == Item::Kind::Separator) {
                continue;
            }
        }
        clean.push_back(std::move(item));
    }
    while (!clean.empty() && clean.back().kind == Item::Kind::Separator) {
        clean.pop_back();
    }
    items = std::move(clean);
}

void apply_shortcuts_rec(std::vector<Item>& items, ShortcutLookup lookup, void* user) {
    for (Item& item : items) {
        if (item.kind == Item::Kind::Command && lookup != nullptr) {
            const std::string_view name = to_string(item.command.id);
            item.shortcut = lookup(user, name, item.command.arg);
        } else if (item.kind == Item::Kind::Submenu) {
            apply_shortcuts_rec(item.children, lookup, user);
        }
    }
}

struct ScoredRow {
    Row row;
    std::size_t order = 0;
};

void collect_matches(const std::vector<Item>& items, search::Matcher& matcher,
                     std::string_view parent_label, std::vector<ScoredRow>& safe_out,
                     std::vector<ScoredRow>& destructive_out, std::size_t& order_counter) {
    for (const Item& item : items) {
        if (item.kind == Item::Kind::Separator || item.kind == Item::Kind::Note || !item.enabled) {
            continue;
        }
        if (item.kind == Item::Kind::Submenu) {
            const std::string_view sub_label = item.label.empty() ? std::string_view(item.label_key)
                                                                  : std::string_view(item.label);
            collect_matches(item.children, matcher, sub_label, safe_out, destructive_out,
                            order_counter);
            continue;
        }
        const std::string_view text =
            item.label.empty() ? std::string_view(item.label_key) : std::string_view(item.label);
        std::optional<std::int32_t> best_score;
        std::vector<search::HighlightRange> highlights;
        if (const auto m = matcher.match(text)) {
            best_score = m->score;
            highlights = m->highlights;
        }
        if (item.kind == Item::Kind::Command) {
            const std::string_view cmd_name = to_string(item.command.id);
            if (const auto s = matcher.score(cmd_name)) {
                const std::int32_t discounted = *s - kMenuCommandNameDiscount;
                if (!best_score || discounted > *best_score) {
                    best_score = discounted;
                    highlights.clear();
                }
            }
        }
        if (!best_score) {
            continue;
        }
        std::int32_t final_score = *best_score;
        if (item.pinned && item.danger != Danger::Destructive) {
            final_score += kPinnedBoost;
        }
        Row row;
        row.item = item;
        row.parent_label = std::string(parent_label);
        row.score = final_score;
        row.highlights = std::move(highlights);
        ScoredRow sr{std::move(row), order_counter++};
        if (item.danger == Danger::Destructive) {
            destructive_out.push_back(std::move(sr));
        } else {
            safe_out.push_back(std::move(sr));
        }
    }
}

} // namespace

std::string_view to_string(MenuKind kind) noexcept {
    switch (kind) {
    case MenuKind::ListRow: return "list_row";
    case MenuKind::ListBackground: return "list_background";
    case MenuKind::Tab: return "tab";
    case MenuKind::TabBar: return "tab_bar";
    case MenuKind::SidebarPlace: return "sidebar_place";
    case MenuKind::SidebarBookmark: return "sidebar_bookmark";
    case MenuKind::SidebarRecents: return "sidebar_recents";
    case MenuKind::TrashRow: return "trash_row";
    case MenuKind::TrashBackground: return "trash_background";
    case MenuKind::Breadcrumb: return "breadcrumb";
    }
    return "list_row";
}

std::vector<Item> build(MenuKind kind, const CommandRegistry& registry,
                        const CommandContext& context, const Target& target) {
    std::vector<Item> items;
    const bool cursor_folder = has(context.flags, ContextFlags::CursorOnFolder);
    const bool closed_tabs = has(context.flags, ContextFlags::ClosedTabsAvailable);

    switch (kind) {
    case MenuKind::ListRow:
        push_if(items, make_cmd(registry, context, CommandId::Activate));
        if (cursor_folder) {
            push_if(items, make_cmd(registry, context, CommandId::OpenInNewTab));
        }
        items.push_back(make_submenu("submenu.open_with", "cmd.open_with.show"));
        push_sep_if_needed(items);
        push_if(items, make_cmd(registry, context, CommandId::Cut));
        push_if(items, make_cmd(registry, context, CommandId::Copy));
        if (cursor_folder) {
            push_if(items, make_cmd(registry, context, CommandId::Paste, PasteArg::IntoCursorFolder,
                                    "cmd.edit.paste.into_folder"));
        }
        push_if(items, make_cmd(registry, context, CommandId::CopyPath));
        push_sep_if_needed(items);
        push_if(items, make_cmd(registry, context, CommandId::Rename));
        push_if(items, make_cmd(registry, context, CommandId::MoveToTrash));
        push_sep_if_needed(items);
        push_if(items, make_cmd(registry, context, CommandId::DeletePermanently));
        break;

    case MenuKind::ListBackground:
        push_if(items, make_cmd(registry, context, CommandId::NewFolder));
        push_if(items, make_cmd(registry, context, CommandId::Paste));
        push_sep_if_needed(items);
        push_if(items, make_cmd(registry, context, CommandId::SelectAll));
        push_if(items, make_cmd(registry, context, CommandId::Refresh));
        push_if(items, make_cmd(registry, context, CommandId::ToggleHidden));
        push_if(items, make_cmd(registry, context, CommandId::CopyPath, CopyPathArg::CurrentFolder,
                                "cmd.edit.copy_path.folder"));
        push_sep_if_needed(items);
        push_if(items, make_cmd(registry, context, CommandId::OpenTrash));
        break;

    case MenuKind::Tab:
        push_if(items, make_cmd(registry, context, CommandId::NewTab, 0));
        push_if(items, make_cmd(registry, context, CommandId::DuplicateTab, target.tab));
        push_if(items, make_cmd(registry, context, CommandId::CloseTab, target.tab));
        push_if(items, make_cmd(registry, context, CommandId::CloseLeftTabs, target.tab));
        if (closed_tabs) {
            push_if(items, make_cmd(registry, context, CommandId::ReopenClosedTab, 0));
        }
        push_if(items, make_cmd(registry, context, CommandId::CopyPath, CopyPathArg::CurrentFolder,
                                "cmd.edit.copy_path.folder"));
        break;

    case MenuKind::TabBar:
        push_if(items, make_cmd(registry, context, CommandId::NewTab, 0));
        if (closed_tabs) {
            push_if(items, make_cmd(registry, context, CommandId::ReopenClosedTab, 0));
        }
        push_if(items, make_cmd(registry, context, CommandId::SearchTabs, 0));
        break;

    case MenuKind::SidebarPlace:
        push_if(items, make_cmd(registry, context, CommandId::Activate));
        push_if(items, make_cmd(registry, context, CommandId::OpenInNewTab));
        break;

    case MenuKind::SidebarBookmark:
        push_if(items, make_cmd(registry, context, CommandId::OpenInNewTab));
        push_if(items, make_cmd(registry, context, CommandId::RenameBookmark, target.bookmark));
        push_if(items, make_cmd(registry, context, CommandId::MoveBookmarkUp, target.bookmark));
        push_if(items, make_cmd(registry, context, CommandId::MoveBookmarkDown, target.bookmark));
        push_if(items, make_cmd(registry, context, CommandId::ToggleBookmark, target.bookmark));
        break;

    case MenuKind::SidebarRecents:
        push_if(items, make_cmd(registry, context, CommandId::ClearRecents));
        push_if(items, make_cmd(registry, context, CommandId::ToggleRecentsTracking));
        break;

    case MenuKind::TrashRow:
        push_if(items, make_cmd(registry, context, CommandId::RestoreFromTrash));
        push_sep_if_needed(items);
        push_if(items, make_cmd(registry, context, CommandId::DeletePermanently));
        push_if(items, make_cmd(registry, context, CommandId::EmptyTrash));
        break;

    case MenuKind::TrashBackground:
        push_if(items, make_cmd(registry, context, CommandId::EmptyTrash));
        break;

    case MenuKind::Breadcrumb:
        push_if(items, make_cmd(registry, context, CommandId::OpenInNewTab));
        push_if(items, make_cmd(registry, context, CommandId::CopyPath, CopyPathArg::CurrentFolder,
                                "cmd.edit.copy_path.folder"));
        break;
    }

    trim_separators(items);
    return items;
}

void apply_shortcuts(std::vector<Item>& items, ShortcutLookup lookup, void* user) {
    apply_shortcuts_rec(items, lookup, user);
}

std::vector<Item> arrange(std::vector<Item> items, const Pins& pins) {
    // Separate destructive items (M2) and pinnable pinned items (M1).
    std::vector<Item> destructive;
    std::vector<Item> remaining;
    remaining.reserve(items.size());
    for (Item& item : items) {
        item.pinned = false;
        if (item.danger == Danger::Destructive) {
            item.pinnable = false;
            destructive.push_back(std::move(item));
        } else {
            remaining.push_back(std::move(item));
        }
    }

    // Extract pinned items in pin order (M1: moved, not duplicated; ignores non-pinnable).
    std::vector<Item> pinned_group;
    for (const std::string& pin_id : pins.ids()) {
        const auto it = std::find_if(remaining.begin(), remaining.end(), [&](const Item& item) {
            return item.pinnable && item.kind == Item::Kind::Command && item.id == pin_id;
        });
        if (it != remaining.end()) {
            it->pinned = true;
            pinned_group.push_back(std::move(*it));
            remaining.erase(it);
        }
    }

    trim_separators(remaining);

    std::vector<Item> out;
    out.reserve(pinned_group.size() + remaining.size() + destructive.size() + 2U);
    if (!pinned_group.empty()) {
        for (Item& item : pinned_group) {
            out.push_back(std::move(item));
        }
    }
    if (!remaining.empty()) {
        if (!out.empty()) {
            out.push_back(make_separator());
        }
        for (Item& item : remaining) {
            out.push_back(std::move(item));
        }
    }
    if (!destructive.empty()) {
        if (!out.empty()) {
            out.push_back(make_separator());
        }
        for (Item& item : destructive) {
            out.push_back(std::move(item));
        }
    }
    trim_separators(out);
    return out;
}

FilterResult filter(const std::vector<Item>& arranged, std::string_view query) {
    FilterResult result;
    const std::string_view trimmed = trim_ascii(query);
    if (trimmed.empty()) {
        result.active = false;
        return result;
    }
    result.active = true;

    search::Matcher matcher(search::compile_query(trimmed, search::MatchMode::Fuzzy));
    std::vector<ScoredRow> safe_rows;
    std::vector<ScoredRow> destructive_rows;
    std::size_t order_counter = 0;
    collect_matches(arranged, matcher, {}, safe_rows, destructive_rows, order_counter);

    const auto cmp = [](const ScoredRow& a, const ScoredRow& b) {
        if (a.row.score != b.row.score) {
            return a.row.score > b.row.score;
        }
        return a.order < b.order;
    };
    std::stable_sort(safe_rows.begin(), safe_rows.end(), cmp);
    std::stable_sort(destructive_rows.begin(), destructive_rows.end(), cmp);

    result.rows.reserve(safe_rows.size() + destructive_rows.size());
    for (ScoredRow& sr : safe_rows) {
        result.rows.push_back(std::move(sr.row));
    }
    const bool had_safe = !result.rows.empty();
    for (std::size_t i = 0; i < destructive_rows.size(); ++i) {
        if (i == 0 && had_safe) {
            destructive_rows[i].row.separated_before = true;
        }
        result.rows.push_back(std::move(destructive_rows[i].row));
    }
    if (had_safe) {
        result.default_row = std::size_t{0};
    }
    return result;
}

} // namespace navigator::menu
