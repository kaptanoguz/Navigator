// Session / named-layout JSON, format version 1 (navigator/layout.hpp "JSON format").
//
// Writing uses nlohmann::ordered_json so keys come out in the documented order; nlohmann prints
// doubles in their shortest round-trip form, so equal sessions give identical text. Reading uses
// nlohmann::json with exceptions disabled and checks every type before reading a value (lenient:
// anything unexpected falls back to its default). JSON nodes that cannot be used (unknown type,
// too deep, not an object) become empty panes, which Layout::from_tree() drops together with
// their size, so children and sizes stay aligned.
#include <algorithm>
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

#include <nlohmann/json.hpp>

#include <navigator/layout.hpp>

#include "utf8.hpp"

namespace navigator::layout {
namespace {

using OutJson = nlohmann::ordered_json;
using InJson = nlohmann::json;

// ---- writing -------------------------------------------------------------------------------

constexpr char kHexDigits[] = "0123456789abcdef";

OutJson bytes_to_json(std::string_view raw) {
    if (core::is_valid_utf8(raw)) {
        // Not braced: {x} would select nlohmann's initializer-list (array) constructor.
        return OutJson(std::string(raw)); // NOLINT(modernize-return-braced-init-list)
    }
    std::string hex;
    hex.reserve(raw.size() * 2);
    for (const char c : raw) {
        const auto b = static_cast<unsigned char>(c);
        hex.push_back(kHexDigits[b >> 4U]);
        hex.push_back(kHexDigits[b & 0x0FU]);
    }
    OutJson out = OutJson::object();
    out["hex"] = std::move(hex);
    return out;
}

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

std::string dump(const OutJson& doc) {
    std::string text = doc.dump(2, ' ', false, OutJson::error_handler_t::strict);
    text.push_back('\n');
    return text;
}

// ---- reading -------------------------------------------------------------------------------

const InJson* member(const InJson& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

// Integer value, saturated to the int64 range. Floats (even 1.0) are not integers.
std::optional<std::int64_t> as_integer(const InJson* value) {
    if (value == nullptr || !value->is_number_integer()) {
        return std::nullopt;
    }
    if (value->is_number_unsigned()) {
        const auto u = value->get<std::uint64_t>();
        constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        return u > kMax ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(u);
    }
    return value->get<std::int64_t>();
}

bool as_bool(const InJson* value, bool fallback) {
    return value != nullptr && value->is_boolean() ? value->get<bool>() : fallback;
}

std::size_t as_index(const InJson* value) {
    const std::optional<std::int64_t> v = as_integer(value);
    if (!v || *v < 0) {
        return 0;
    }
    return static_cast<std::size_t>(*v);
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::optional<std::string> as_bytes(const InJson* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->is_string()) {
        return value->get<std::string>();
    }
    const InJson* hex = member(*value, "hex");
    if (hex == nullptr || !hex->is_string()) {
        return std::nullopt;
    }
    const auto& digits = hex->get_ref<const std::string&>();
    if (digits.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string raw;
    raw.reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        const int hi = hex_value(digits[i]);
        const int lo = hex_value(digits[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        raw.push_back(static_cast<char>((hi << 4) | lo));
    }
    return raw;
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

Result<InJson> parse_object(std::string_view text) {
    InJson doc = InJson::parse(text.begin(), text.end(), nullptr, false);
    if (doc.is_discarded()) {
        return fail(ErrorCode::InvalidArgument, "not valid JSON");
    }
    if (!doc.is_object()) {
        return fail(ErrorCode::InvalidArgument, "top-level JSON value is not an object");
    }
    return doc;
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
