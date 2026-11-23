// Session / named-layout JSON, format version 1 (navigator/layout.hpp "JSON format").
//
// Writing uses nlohmann::ordered_json so keys come out in the documented order; nlohmann prints
// doubles in their shortest round-trip form, so equal sessions give identical text. Reading uses
// nlohmann::json with exceptions disabled and checks every type before reading a value (lenient:
// anything unexpected falls back to its default). JSON nodes that cannot be used (unknown type,
// too deep, not an object) become empty panes, which Layout::from_tree() drops together with
// their size, so children and sizes stay aligned.
//
// "hidden_types" (Phase 5): src/core does not link src/search, so normalized_types() below
// re-implements search::normalize(TypeFilter) from its documented rule (search.hpp TypeFilter /
// layout.hpp "hidden_types"): drop empty or '/'- or NUL-containing items, fold with
// fs::fold_name, sort by bytes, unique. The writer normalizes a copy too, so a filter the loader
// would change is written as it would be read back (save -> load -> save is stable).
//
// "inspector" (Phase 6): the writer normalizes the state the same way (sizes clamped, unknown
// placement -> Auto) and omits the field when the normalized state equals InspectorState{}.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/layout.hpp>
#include <navigator/search.hpp>

#include "json_util.hpp"

namespace navigator::layout {
namespace {

using core::json::as_bool;
using core::json::as_bytes;
using core::json::as_integer;
using core::json::bytes_to_json;
using core::json::dump;
using core::json::InJson;
using core::json::member;
using core::json::OutJson;
using core::json::parse_object;

// ---- hidden types (Phase 5) ----------------------------------------------------------------

// Mirrors search::normalize(TypeFilter) (see the file comment).
search::TypeFilter normalized_types(search::TypeFilter filter) {
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
    return filter;
}

// ---- writing -------------------------------------------------------------------------------

const char* sort_key_name(fs::SortKey key) noexcept {
    switch (key) {
    case fs::SortKey::Name: return "name";
    case fs::SortKey::Size: return "size";
    case fs::SortKey::Modified: return "modified";
    case fs::SortKey::Extension: return "extension";
    case fs::SortKey::Count: return "count";
    }
    return "name";
}

OutJson tab_to_json(const TabState& tab) {
    OutJson out = OutJson::object();
    out["path"] = bytes_to_json(tab.path.native());
    out["show_hidden"] = tab.show_hidden;
    if (!tab.cursor_name.empty()) {
        out["cursor"] = bytes_to_json(tab.cursor_name);
    }
    // A value the loader would drop is not written (keeps save -> load -> save stable).
    if (tab.scroll_rows && std::isfinite(*tab.scroll_rows) && *tab.scroll_rows >= 0.0) {
        out["scroll"] = *tab.scroll_rows;
    }
    OutJson sort = OutJson::object();
    sort["key"] = sort_key_name(tab.sort.key);
    sort["descending"] = tab.sort.descending;
    sort["folders_first"] = tab.sort.folders_first;
    sort["natural"] = tab.sort.natural;
    sort["case_insensitive"] = tab.sort.case_insensitive;
    out["sort"] = std::move(sort);
    if (const search::TypeFilter types = normalized_types(tab.type_filter); types.active()) {
        OutJson hidden = OutJson::object();
        if (types.hide_folders) {
            hidden["folders"] = true;
        }
        if (types.hide_no_extension) {
            hidden["no_extension"] = true;
        }
        if (!types.hidden_extensions.empty()) {
            OutJson extensions = OutJson::array();
            for (const std::string& ext : types.hidden_extensions) {
                extensions.push_back(bytes_to_json(ext));
            }
            hidden["extensions"] = std::move(extensions);
        }
        out["hidden_types"] = std::move(hidden);
    }
    return out;
}

OutJson node_to_json(const LayoutNode& node) {
    OutJson out = OutJson::object();
    if (const auto* pane = std::get_if<PaneNode>(&node.node)) {
        out["type"] = "pane";
        out["active_tab"] = pane->active_tab;
        OutJson tabs = OutJson::array();
        for (const Tab& tab : pane->tabs) {
            tabs.push_back(tab_to_json(tab.state));
        }
        out["tabs"] = std::move(tabs);
        return out;
    }
    const auto& split = std::get<SplitNode>(node.node);
    out["type"] = "split";
    out["axis"] = split.axis == SplitAxis::Vertical ? "vertical" : "horizontal";
    OutJson sizes = OutJson::array();
    for (const double size : split.sizes) {
        sizes.push_back(size);
    }
    out["sizes"] = std::move(sizes);
    OutJson children = OutJson::array();
    for (const LayoutNode& child : split.children) {
        children.push_back(node_to_json(child));
    }
    out["children"] = std::move(children);
    return out;
}

OutJson layout_to_value(const Layout& layout) {
    const std::vector<PaneId> order = layout.pane_order();
    const auto it = std::find(order.begin(), order.end(), layout.active_pane());
    OutJson out = OutJson::object();
    out["active_pane"] =
        it == order.end() ? std::size_t{0} : static_cast<std::size_t>(it - order.begin());
    out["root"] = node_to_json(layout.root());
    return out;
}

// ---- reading -------------------------------------------------------------------------------

std::size_t as_index(const InJson* value) {
    const std::optional<std::int64_t> v = as_integer(value);
    if (!v || *v < 0) {
        return 0;
    }
    return static_cast<std::size_t>(*v);
}

fs::SortKey sort_key_from(const InJson* value) {
    if (value == nullptr || !value->is_string()) {
        return fs::SortKey::Name;
    }
    const auto& name = value->get_ref<const std::string&>();
    if (name == "size") {
        return fs::SortKey::Size;
    }
    if (name == "modified") {
        return fs::SortKey::Modified;
    }
    if (name == "extension") {
        return fs::SortKey::Extension;
    }
    if (name == "count") {
        return fs::SortKey::Count;
    }
    return fs::SortKey::Name;
}

// A tab with an empty path when `value` is unusable (from_tree / closed_tabs drop it).
TabState tab_from(const InJson& value) {
    TabState tab;
    if (!value.is_object()) {
        return tab;
    }
    if (std::optional<std::string> path = as_bytes(member(value, "path"))) {
        tab.path = Path(std::move(*path));
    }
    tab.show_hidden = as_bool(member(value, "show_hidden"), false);
    if (std::optional<std::string> cursor = as_bytes(member(value, "cursor"))) {
        tab.cursor_name = std::move(*cursor);
    }
    if (const InJson* scroll = member(value, "scroll"); scroll != nullptr && scroll->is_number()) {
        const auto rows = scroll->get<double>();
        if (std::isfinite(rows) && rows >= 0.0) {
            tab.scroll_rows = rows;
        }
    }
    if (const InJson* sort = member(value, "sort"); sort != nullptr && sort->is_object()) {
        const fs::SortOptions defaults;
        tab.sort.key = sort_key_from(member(*sort, "key"));
        tab.sort.descending = as_bool(member(*sort, "descending"), defaults.descending);
        tab.sort.folders_first = as_bool(member(*sort, "folders_first"), defaults.folders_first);
        tab.sort.natural = as_bool(member(*sort, "natural"), defaults.natural);
        tab.sort.case_insensitive =
            as_bool(member(*sort, "case_insensitive"), defaults.case_insensitive);
    }
    if (const InJson* hidden = member(value, "hidden_types");
        hidden != nullptr && hidden->is_object()) {
        search::TypeFilter types;
        types.hide_folders = as_bool(member(*hidden, "folders"), false);
        types.hide_no_extension = as_bool(member(*hidden, "no_extension"), false);
        if (const InJson* extensions = member(*hidden, "extensions");
            extensions != nullptr && extensions->is_array()) {
            for (const InJson& item : *extensions) {
                if (std::optional<std::string> ext = as_bytes(&item)) {
                    types.hidden_extensions.push_back(std::move(*ext));
                }
            }
        }
        tab.type_filter = normalized_types(std::move(types));
    }
    return tab;
}

LayoutNode dropped_node() { return LayoutNode{PaneNode{}}; }

LayoutNode node_from(const InJson& value, std::size_t depth) {
    if (depth > kMaxTreeDepth || !value.is_object()) {
        return dropped_node();
    }
    const InJson* type = member(value, "type");
    if (type == nullptr || !type->is_string()) {
        return dropped_node();
    }
    const auto& kind = type->get_ref<const std::string&>();
    if (kind == "pane") {
        PaneNode pane;
        if (const InJson* tabs = member(value, "tabs"); tabs != nullptr && tabs->is_array()) {
            pane.tabs.reserve(tabs->size());
            for (const InJson& tab : *tabs) {
                pane.tabs.push_back(Tab{TabId{}, tab_from(tab)});
            }
        }
        pane.active_tab = as_index(member(value, "active_tab"));
        return LayoutNode{std::move(pane)};
    }
    if (kind != "split") {
        return dropped_node();
    }
    SplitNode split;
    const InJson* axis = member(value, "axis");
    if (axis == nullptr || !axis->is_string()) {
        return dropped_node();
    }
    if (axis->get_ref<const std::string&>() == "horizontal") {
        split.axis = SplitAxis::Horizontal;
    } else if (axis->get_ref<const std::string&>() == "vertical") {
        split.axis = SplitAxis::Vertical;
    } else {
        return dropped_node();
    }
    if (const InJson* sizes = member(value, "sizes"); sizes != nullptr && sizes->is_array()) {
        split.sizes.reserve(sizes->size());
        for (const InJson& size : *sizes) {
            split.sizes.push_back(size.is_number() ? size.get<double>()
                                                   : std::numeric_limits<double>::quiet_NaN());
        }
    }
    if (const InJson* children = member(value, "children");
        children != nullptr && children->is_array()) {
        split.children.reserve(children->size());
        for (const InJson& child : *children) {
            split.children.push_back(node_from(child, depth + 1));
        }
    }
    return LayoutNode{std::move(split)};
}

Layout layout_from_value(const InJson* value) {
    if (value == nullptr || !value->is_object()) {
        return {};
    }
    const InJson* root = member(*value, "root");
    if (root == nullptr || !root->is_object()) {
        return {};
    }
    return Layout::from_tree(node_from(*root, 1), as_index(member(*value, "active_pane")));
}

WindowGeometry window_from(const InJson* value) {
    WindowGeometry window;
    if (value == nullptr || !value->is_object()) {
        return window;
    }
    // The integer value when it lies in [lo, hi].
    const auto in_range = [&](const char* key, std::int64_t lo,
                              std::int64_t hi) -> std::optional<int> {
        const std::optional<std::int64_t> v = as_integer(member(*value, key));
        if (v.has_value() && *v >= lo && *v <= hi) {
            return static_cast<int>(*v);
        }
        return std::nullopt;
    };
    window.width = in_range("width", 1, 32768).value_or(window.width);
    window.height = in_range("height", 1, 32768).value_or(window.height);
    const std::optional<int> x = in_range("x", -32768, 32768);
    const std::optional<int> y = in_range("y", -32768, 32768);
    if (x.has_value() && y.has_value()) {
        window.has_position = true;
        window.x = *x;
        window.y = *y;
    }
    window.maximized = as_bool(member(*value, "maximized"), false);
    return window;
}

// ---- sidebar (Phase 4) ---------------------------------------------------------------------

constexpr std::array<const char*, kSidebarSectionCount> kSectionNames = {"bookmarks", "recents",
                                                                         "places", "storage"};

double clamp_width(double width) noexcept {
    if (!std::isfinite(width)) {
        return kSidebarDefaultWidth;
    }
    return std::clamp(width, kSidebarMinWidth, kSidebarMaxWidth);
}

OutJson sidebar_to_json(const SidebarState& sidebar) {
    OutJson out = OutJson::object();
    out["visible"] = sidebar.visible;
    // The loader clamps; write what it would read back so save -> load -> save is stable.
    const double width = clamp_width(sidebar.width);
    if (width == std::floor(width)) {
        out["width"] = static_cast<std::int64_t>(width); // integral: no ".0"
    } else {
        out["width"] = width;
    }
    OutJson collapsed = OutJson::array();
    for (std::size_t i = 0; i < kSidebarSectionCount; ++i) {
        if (sidebar.collapsed[i]) {
            collapsed.push_back(kSectionNames[i]);
        }
    }
    out["collapsed"] = std::move(collapsed);
    return out;
}

SidebarState sidebar_from(const InJson* value) {
    SidebarState sidebar;
    if (value == nullptr || !value->is_object()) {
        return sidebar;
    }
    sidebar.visible = as_bool(member(*value, "visible"), true);
    if (const InJson* width = member(*value, "width"); width != nullptr && width->is_number()) {
        sidebar.width = clamp_width(width->get<double>());
    }
    if (const InJson* collapsed = member(*value, "collapsed");
        collapsed != nullptr && collapsed->is_array()) {
        for (const InJson& item : *collapsed) {
            if (!item.is_string()) {
                continue;
            }
            const auto& name = item.get_ref<const std::string&>();
            for (std::size_t i = 0; i < kSidebarSectionCount; ++i) {
                if (name == kSectionNames[i]) {
                    sidebar.collapsed[i] = true;
                }
            }
        }
    }
    return sidebar;
}

// ---- inspector (Phase 6) -------------------------------------------------------------------

double clamp_extent(double value, double fallback, double lo, double hi) noexcept {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, lo, hi);
}

// The state as the loader would read it back: sizes clamped, an out-of-range enum -> Auto.
InspectorState normalized_inspector(InspectorState state) noexcept {
    if (state.placement != InspectorPlacement::Right &&
        state.placement != InspectorPlacement::Bottom) {
        state.placement = InspectorPlacement::Auto;
    }
    state.width =
        clamp_extent(state.width, kInspectorDefaultWidth, kInspectorMinWidth, kInspectorMaxWidth);
    state.height = clamp_extent(state.height, kInspectorDefaultHeight, kInspectorMinHeight,
                                kInspectorMaxHeight);
    return state;
}

const char* placement_name(InspectorPlacement placement) noexcept {
    switch (placement) {
    case InspectorPlacement::Auto: return "auto";
    case InspectorPlacement::Right: return "right";
    case InspectorPlacement::Bottom: return "bottom";
    }
    return "auto";
}

// Integral values without a fraction, like the sidebar width.
OutJson number_to_json(double value) {
    if (value == std::floor(value)) {
        return static_cast<std::int64_t>(value);
    }
    return value;
}

// Every field, in the documented order. The caller writes it only when != InspectorState{}.
OutJson inspector_to_json(const InspectorState& state) {
    OutJson out = OutJson::object();
    out["visible"] = state.visible;
    out["placement"] = placement_name(state.placement);
    out["width"] = number_to_json(state.width);
    out["height"] = number_to_json(state.height);
    out["grid"] = state.show_grid;
    out["minimap"] = state.show_minimap;
    return out;
}

InspectorState inspector_from(const InJson* value) {
    InspectorState state;
    if (value == nullptr || !value->is_object()) {
        return state;
    }
    const InspectorState defaults;
    state.visible = as_bool(member(*value, "visible"), defaults.visible);
    if (const InJson* placement = member(*value, "placement");
        placement != nullptr && placement->is_string()) {
        const auto& name = placement->get_ref<const std::string&>();
        if (name == "right") {
            state.placement = InspectorPlacement::Right;
        } else if (name == "bottom") {
            state.placement = InspectorPlacement::Bottom;
        }
    }
    if (const InJson* width = member(*value, "width"); width != nullptr && width->is_number()) {
        state.width = clamp_extent(width->get<double>(), kInspectorDefaultWidth, kInspectorMinWidth,
                                   kInspectorMaxWidth);
    }
    if (const InJson* height = member(*value, "height"); height != nullptr && height->is_number()) {
        state.height = clamp_extent(height->get<double>(), kInspectorDefaultHeight,
                                    kInspectorMinHeight, kInspectorMaxHeight);
    }
    state.show_grid = as_bool(member(*value, "grid"), defaults.show_grid);
    state.show_minimap = as_bool(member(*value, "minimap"), defaults.show_minimap);
    return state;
}

} // namespace

Result<std::string> to_json(const Session& session) {
    try {
        OutJson doc = OutJson::object();
        doc["version"] = Session::kVersion;
        OutJson window = OutJson::object();
        if (session.window.has_position) {
            window["x"] = session.window.x;
            window["y"] = session.window.y;
        }
        window["width"] = session.window.width;
        window["height"] = session.window.height;
        window["maximized"] = session.window.maximized;
        doc["window"] = std::move(window);
        doc["layout"] = layout_to_value(session.layout);
        OutJson closed = OutJson::array();
        for (const ClosedTab& tab : session.closed_tabs.entries()) {
            closed.push_back(tab_to_json(tab.state));
        }
        doc["closed_tabs"] = std::move(closed);
        doc["sidebar"] = sidebar_to_json(session.sidebar);
        // Absent when default, so files of users who never open the Inspector are unchanged.
        if (const InspectorState inspector = normalized_inspector(session.inspector);
            inspector != InspectorState{}) {
            doc["inspector"] = inspector_to_json(inspector);
        }
        return dump(doc);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while serializing the session");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Unknown, std::string("session serialization failed: ") + e.what());
    }
}

Result<Session> session_from_json(std::string_view text) {
    try {
        Result<InJson> doc = parse_object(text);
        if (!doc) {
            return Unexpected<Error>(doc.error());
        }
        Session session;
        const std::optional<std::int64_t> version = as_integer(member(*doc, "version"));
        if (version && *version >= 1) {
            session.version =
                static_cast<int>(std::min<std::int64_t>(*version, std::numeric_limits<int>::max()));
        }
        session.window = window_from(member(*doc, "window"));
        session.layout = layout_from_value(member(*doc, "layout"));
        if (const InJson* closed = member(*doc, "closed_tabs");
            closed != nullptr && closed->is_array()) {
            std::vector<TabState> kept;
            for (const InJson& entry : *closed) {
                TabState tab = tab_from(entry);
                if (!tab.path.empty() && tab.path.is_absolute()) {
                    kept.push_back(std::move(tab));
                }
            }
            const std::size_t first = kept.size() > kDefaultClosedTabCapacity
                                          ? kept.size() - kDefaultClosedTabCapacity
                                          : 0;
            for (std::size_t i = first; i < kept.size(); ++i) {
                session.closed_tabs.push(ClosedTab{std::move(kept[i]), PaneId{}, 0});
            }
        }
        session.sidebar = sidebar_from(member(*doc, "sidebar"));
        session.inspector = inspector_from(member(*doc, "inspector"));
        return session;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while reading the session");
    } catch (const std::exception& e) {
        return fail(ErrorCode::InvalidArgument, std::string("session JSON rejected: ") + e.what());
    }
}

Result<std::string> layout_to_json(const Layout& layout) {
    try {
        OutJson doc = OutJson::object();
        doc["version"] = Session::kVersion;
        doc["layout"] = layout_to_value(layout);
        return dump(doc);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while serializing the layout");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Unknown, std::string("layout serialization failed: ") + e.what());
    }
}

Result<Layout> layout_from_json(std::string_view text) {
    try {
        Result<InJson> doc = parse_object(text);
        if (!doc) {
            return Unexpected<Error>(doc.error());
        }
        return layout_from_value(member(*doc, "layout"));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while reading the layout");
    } catch (const std::exception& e) {
        return fail(ErrorCode::InvalidArgument, std::string("layout JSON rejected: ") + e.what());
    }
}

} // namespace navigator::layout
