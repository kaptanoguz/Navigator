// src/ui/workspace/tab_search.cpp — see tab_search.hpp.
#include "workspace/tab_search.hpp"

#include <algorithm>
#include <variant>

#include <navigator/fs.hpp>
#include <navigator/i18n.hpp>

#include "browser/format.hpp"

namespace navigator::ui {
namespace {

std::vector<std::string> words(std::string_view q) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && q[i] == ' ') {
            ++i;
        }
        std::size_t j = i;
        while (j < q.size() && q[j] != ' ') {
            ++j;
        }
        if (j > i) {
            out.push_back(fs::fold_name(q.substr(i, j - i)));
        }
        i = j;
    }
    return out;
}

void collect_panes(const layout::LayoutNode& node, std::vector<const layout::PaneNode*>& out) {
    if (const auto* p = std::get_if<layout::PaneNode>(&node.node)) {
        out.push_back(p);
        return;
    }
    for (const layout::LayoutNode& c : std::get<layout::SplitNode>(node.node).children) {
        collect_panes(c, out);
    }
}

} // namespace

std::string tab_title(const Path& dir) {
    const Path name = dir.filename();
    if (name.empty() || name == ".") {
        if (dir.empty() || dir == dir.root_path()) {
            return std::string(tr("path.root"));
        }
        return display_name(dir.parent_path().filename().string());
    }
    return display_name(name.string());
}

std::string tab_detail(const Path& dir, const Path* home) {
    const std::string s = dir.string();
    if (home != nullptr && !home->empty() && *home != home->root_path()) {
        const std::string h = home->string();
        if (s == h) {
            return "~";
        }
        if (s.size() > h.size() && s.compare(0, h.size(), h) == 0 && s[h.size()] == '/') {
            return display_name("~" + s.substr(h.size()));
        }
    }
    return display_name(s);
}

std::vector<TabSearchItem> collect_tab_search_items(const layout::Layout& layout,
                                                    const layout::ClosedTabs& closed,
                                                    const Path* home) {
    std::vector<TabSearchItem> out;
    std::vector<const layout::PaneNode*> panes;
    collect_panes(layout.root(), panes);
    const layout::TabId active = layout.active_tab();
    for (std::size_t p = 0; p < panes.size(); ++p) {
        for (const layout::Tab& t : panes[p]->tabs) {
            TabSearchItem item;
            item.source = TabSearchItem::Source::Open;
            item.tab = t.id;
            item.pane = panes[p]->id;
            item.pane_number = p + 1;
            item.active = t.id == active;
            item.title = tab_title(t.state.path);
            item.detail = tab_detail(t.state.path, home);
            out.push_back(std::move(item));
        }
    }
    const Span<const layout::ClosedTab> entries = closed.entries();
    for (std::size_t k = entries.size(); k > 0; --k) {
        const layout::ClosedTab& c = entries[k - 1];
        TabSearchItem item;
        item.source = TabSearchItem::Source::Closed;
        item.closed_index = k - 1;
        item.title = tab_title(c.state.path);
        item.detail = tab_detail(c.state.path, home);
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<std::size_t> filter_tab_search(const std::vector<TabSearchItem>& items,
                                           std::string_view query) {
    const std::vector<std::string> ws = words(query);
    std::vector<std::size_t> head;
    std::vector<std::size_t> tail;
    std::vector<std::size_t> out;
    TabSearchItem::Source group = TabSearchItem::Source::Open;
    const auto flush = [&] {
        out.insert(out.end(), head.begin(), head.end());
        out.insert(out.end(), tail.begin(), tail.end());
        head.clear();
        tail.clear();
    };
    for (std::size_t i = 0; i < items.size(); ++i) {
        const TabSearchItem& it = items[i];
        if (it.source != group) {
            flush();
            group = it.source;
        }
        if (ws.empty()) {
            tail.push_back(i);
            continue;
        }
        const std::string title = fs::fold_name(it.title);
        const std::string detail = fs::fold_name(it.detail);
        const bool all = std::all_of(ws.begin(), ws.end(), [&](const std::string& w) {
            return title.find(w) != std::string::npos || detail.find(w) != std::string::npos;
        });
        if (!all) {
            continue;
        }
        if (title.starts_with(ws.front())) {
            head.push_back(i);
        } else {
            tail.push_back(i);
        }
    }
    flush();
    return out;
}

} // namespace navigator::ui
