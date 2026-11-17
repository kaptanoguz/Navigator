// tests/core/layout_test_util.hpp: helpers shared by the navigator::layout tests.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <navigator/layout.hpp>

namespace navigator::test {

namespace lay = navigator::layout;

inline lay::TabState tab_at(const std::string& path) {
    lay::TabState state;
    state.path = path;
    return state;
}

inline const lay::SplitNode& split_of(const lay::LayoutNode& node) {
    return std::get<lay::SplitNode>(node.node);
}

inline const lay::PaneNode& pane_of(const lay::LayoutNode& node) {
    return std::get<lay::PaneNode>(node.node);
}

inline bool is_pane(const lay::LayoutNode& node) {
    return std::holds_alternative<lay::PaneNode>(node.node);
}

// Paths of a pane's tabs, in order.
inline std::vector<std::string> tab_paths(const lay::Layout& layout, lay::PaneId pane) {
    std::vector<std::string> paths;
    if (const lay::PaneNode* node = layout.find_pane(pane)) {
        for (const lay::Tab& tab : node->tabs) {
            paths.push_back(tab.state.path.native());
        }
    }
    return paths;
}

// Location of a tab that must exist (invalid pane / index 0 when it does not).
inline lay::TabLocation location_of(const lay::Layout& layout, lay::TabId tab) {
    return layout.find_tab(tab).value_or(lay::TabLocation{});
}

inline std::size_t depth_of(const lay::LayoutNode& node) {
    if (is_pane(node)) {
        return 1;
    }
    std::size_t deepest = 0;
    for (const lay::LayoutNode& child : split_of(node).children) {
        deepest = std::max(deepest, depth_of(child));
    }
    return deepest + 1;
}

// Deep equality of two layouts, ids included.
inline bool same_node(const lay::LayoutNode& a, const lay::LayoutNode& b) {
    if (a.node.index() != b.node.index()) {
        return false;
    }
    if (is_pane(a)) {
        const lay::PaneNode& pa = pane_of(a);
        const lay::PaneNode& pb = pane_of(b);
        if (pa.id != pb.id || pa.active_tab != pb.active_tab || pa.tabs.size() != pb.tabs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < pa.tabs.size(); ++i) {
            if (pa.tabs[i].id != pb.tabs[i].id || !(pa.tabs[i].state == pb.tabs[i].state)) {
                return false;
            }
        }
        return true;
    }
    const lay::SplitNode& sa = split_of(a);
    const lay::SplitNode& sb = split_of(b);
    if (sa.id != sb.id || sa.axis != sb.axis || sa.sizes != sb.sizes ||
        sa.children.size() != sb.children.size()) {
        return false;
    }
    for (std::size_t i = 0; i < sa.children.size(); ++i) {
        if (!same_node(sa.children[i], sb.children[i])) {
            return false;
        }
    }
    return true;
}

inline bool same_layout(const lay::Layout& a, const lay::Layout& b) {
    return a.active_pane() == b.active_pane() && same_node(a.root(), b.root());
}

} // namespace navigator::test
