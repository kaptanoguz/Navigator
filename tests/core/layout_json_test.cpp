// navigator::layout JSON v1: round trip, deterministic golden output, lenient loading, raw-byte
// (non-UTF-8) paths and hostile input.
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/layout.hpp>

#include "layout_test_util.hpp"

using namespace navigator;
using navigator::test::is_pane;
using navigator::test::pane_of;
using navigator::test::split_of;
using navigator::test::tab_at;
using navigator::test::tab_paths;
namespace lay = navigator::layout;

namespace {

// Three panes (H[A, V[B, C]]) holding three tabs in A; varied tab state; closed tabs; X11 window.
lay::Session three_pane_session() {
    lay::Session session;
    session.window = lay::WindowGeometry{true, -1920, 40, 1600, 900, true};
    lay::TabState home = tab_at("/home/u");
    home.sort.key = fs::SortKey::Modified;
    home.sort.descending = true;
    home.show_hidden = true;
    home.cursor_name = "Belgeler";
    home.scroll_rows = 12.5;
    session.layout = lay::Layout(home);
    const lay::PaneId a = session.layout.active_pane();
    lay::TabState docs = tab_at("/home/u/Çalışma Alanı");
    docs.sort.folders_first = false;
    docs.sort.natural = false;
    docs.sort.case_insensitive = false;
    REQUIRE(session.layout.add_tab(a, docs, 1, false));
    REQUIRE(session.layout.add_tab(a, tab_at("/srv"), 2, false));
    const lay::PaneId b = session.layout.split_pane(a, lay::SplitEdge::Right, tab_at("/tmp"))->pane;
    lay::TabState music = tab_at("/home/u/Müzik");
    music.sort.key = fs::SortKey::Size;
    const lay::PaneId c = session.layout.split_pane(b, lay::SplitEdge::Bottom, music)->pane;
    REQUIRE(session.layout.move_splitter(split_of(session.layout.root()).id, 0, 0.375));
    REQUIRE(session.layout.activate_pane(c));
    session.closed_tabs.push(lay::ClosedTab{tab_at("/opt"), a, 1});
    session.closed_tabs.push(lay::ClosedTab{tab_at("/var/log"), b, 0});
    return session;
}

lay::Session must_load(const std::string& text) {
    Result<lay::Session> session = lay::session_from_json(text);
    INFO(text);
    REQUIRE(session);
    REQUIRE(session->layout.validate());
    return std::move(*session);
}

} // namespace

TEST_CASE("json: a three-pane session round-trips", "[layout][json]") {
    const lay::Session original = three_pane_session();
    const Result<std::string> text = lay::to_json(original);
    REQUIRE(text);
    const lay::Session back = must_load(*text);

    CHECK(back.version == 1);
    CHECK(back.window == original.window);
    CHECK(back.layout.pane_count() == 3);
    CHECK(back.layout.tab_count() == 5);
    const std::vector<lay::PaneId> order = back.layout.pane_order();
    const std::vector<lay::PaneId> original_order = original.layout.pane_order();
    REQUIRE(order.size() == 3);
    CHECK(back.layout.active_pane() == order[2]);
    for (std::size_t i = 0; i < order.size(); ++i) {
        const lay::PaneNode* got = back.layout.find_pane(order[i]);
        const lay::PaneNode* want = original.layout.find_pane(original_order[i]);
        REQUIRE(got->tabs.size() == want->tabs.size());
        CHECK(got->active_tab == want->active_tab);
        for (std::size_t t = 0; t < got->tabs.size(); ++t) {
            CHECK(got->tabs[t].state == want->tabs[t].state);
        }
    }
    const lay::SplitNode& root = split_of(back.layout.root());
    CHECK(root.sizes == split_of(original.layout.root()).sizes);
    CHECK(split_of(root.children[1]).axis == lay::SplitAxis::Vertical);
    REQUIRE(back.closed_tabs.size() == 2);
    CHECK(back.closed_tabs.entries()[0].state.path == "/opt");
    CHECK_FALSE(back.closed_tabs.entries()[0].pane.valid());
    CHECK(back.closed_tabs.entries()[0].index == 0);

    // Deterministic: the reloaded session serializes to identical text.
    const Result<std::string> again = lay::to_json(back);
    REQUIRE(again);
    CHECK(*again == *text);
    CHECK(*lay::to_json(original) == *text);
}

TEST_CASE("json: golden output", "[layout][json]") {
    lay::Session session;
    lay::TabState home = tab_at("/home/u");
    home.scroll_rows = 3.0;
    session.layout = lay::Layout(home);
    REQUIRE(session.layout
                .split_pane(session.layout.active_pane(), lay::SplitEdge::Bottom,
                            tab_at(std::string("/raw/\xff\xfe")))
                .has_value());
    session.closed_tabs.push(lay::ClosedTab{tab_at("/opt"), {}, 0});
    const Result<std::string> text = lay::to_json(session);
    REQUIRE(text);
    const std::string golden = R"({
  "version": 1,
  "window": {
    "width": 1280,
    "height": 800,
    "maximized": false
  },
  "layout": {
    "active_pane": 1,
    "root": {
      "type": "split",
      "axis": "vertical",
      "sizes": [
        0.5,
        0.5
      ],
      "children": [
        {
          "type": "pane",
          "active_tab": 0,
          "tabs": [
            {
              "path": "/home/u",
              "show_hidden": false,
              "scroll": 3.0,
              "sort": {
                "key": "name",
                "descending": false,
                "folders_first": true,
                "natural": true,
                "case_insensitive": true
              }
            }
          ]
        },
        {
          "type": "pane",
          "active_tab": 0,
          "tabs": [
            {
              "path": {
                "hex": "2f7261772ffffe"
              },
              "show_hidden": false,
              "sort": {
                "key": "name",
                "descending": false,
                "folders_first": true,
                "natural": true,
                "case_insensitive": true
              }
            }
          ]
        }
      ]
    }
  },
  "closed_tabs": [
    {
      "path": "/opt",
      "show_hidden": false,
      "sort": {
        "key": "name",
        "descending": false,
        "folders_first": true,
        "natural": true,
        "case_insensitive": true
      }
    }
  ],
  "sidebar": {
    "visible": true,
    "width": 220,
    "collapsed": []
  }
}
)";
    CHECK(*text == golden);

    lay::Layout single(tab_at("/a"));
    const Result<std::string> named = lay::layout_to_json(single);
    REQUIRE(named);
    CHECK(named->starts_with("{\n  \"version\": 1,\n  \"layout\": {\n    \"active_pane\": 0,"));
}

TEST_CASE("json: non-UTF-8 paths and cursor names round-trip as hex", "[layout][json]") {
    const std::string raw_path = std::string("/data/caf\xe9/\x80\x01z");
    const std::string raw_cursor = std::string("\xc3\x28name");
    lay::TabState state = tab_at(raw_path);
    state.cursor_name = raw_cursor;
    lay::Session session;
    session.layout = lay::Layout(state);
    session.closed_tabs.push(lay::ClosedTab{tab_at(std::string("/x\xff")), {}, 0});
    const Result<std::string> text = lay::to_json(session);
    REQUIRE(text);
    CHECK(text->find("\"hex\"") != std::string::npos);
    const lay::Session back = must_load(*text);
    const lay::TabState* got = back.layout.tab_state(back.layout.active_tab());
    REQUIRE(got != nullptr);
    CHECK(got->path.native() == raw_path);
    CHECK(got->cursor_name == raw_cursor);
    CHECK(back.closed_tabs.entries()[0].state.path.native() == std::string("/x\xff"));

    // Valid UTF-8 (Turkish, emoji) stays a plain string.
    lay::Session plain;
    plain.layout = lay::Layout(tab_at("/home/oğuz/İstanbul 📁"));
    const Result<std::string> plain_text = lay::to_json(plain);
    REQUIRE(plain_text);
    CHECK(plain_text->find("\"/home/oğuz/İstanbul 📁\"") != std::string::npos);
    CHECK(
        must_load(*plain_text).layout.tab_state(must_load(*plain_text).layout.active_tab())->path ==
        "/home/oğuz/İstanbul 📁");

    // Uppercase hex is accepted; odd length or non-hex counts as missing (tab dropped).
    const lay::Session upper = must_load(
        R"({"layout":{"root":{"type":"pane","tabs":[{"path":{"hex":"2F41"}},{"path":{"hex":"2f4"}},{"path":{"hex":"zz"}},{"path":5}]}}})");
    CHECK(tab_paths(upper.layout, upper.layout.active_pane()) == std::vector<std::string>{"/A"});
}

TEST_CASE("json: top level must be an object", "[layout][json]") {
    for (const char* text : {"", "   ", "[1, 2]", "42", "\"session\"", "null", "{", "{\"a\":}",
                             "garbage", "{} trailing"}) {
        INFO(text);
        const Result<lay::Session> session = lay::session_from_json(text);
        REQUIRE_FALSE(session);
        CHECK(session.error().code == ErrorCode::InvalidArgument);
        const Result<lay::Layout> layout = lay::layout_from_json(text);
        REQUIRE_FALSE(layout);
        CHECK(layout.error().code == ErrorCode::InvalidArgument);
    }
    // Invalid UTF-8 inside the JSON text is not JSON.
    CHECK_FALSE(lay::session_from_json(std::string("{\"a\":\"\xff\"}")));
}

TEST_CASE("json: an empty object gives defaults", "[layout][json]") {
    const lay::Session session = must_load("{}");
    CHECK(session.version == 1);
    CHECK(session.window == lay::WindowGeometry{});
    CHECK(session.layout.pane_count() == 1);
    CHECK(session.layout.tab_count() == 0);
    CHECK(session.closed_tabs.empty());
    const Result<lay::Layout> layout = lay::layout_from_json("{}");
    REQUIRE(layout);
    CHECK(layout->tab_count() == 0);
}

TEST_CASE("json: version handling", "[layout][json]") {
    CHECK(must_load(R"({"version": 7})").version == 7);
    CHECK(must_load(R"({"version": 0})").version == 1);
    CHECK(must_load(R"({"version": -3})").version == 1);
    CHECK(must_load(R"({"version": "2"})").version == 1);
    CHECK(must_load(R"({"version": 2.0})").version == 1);
    CHECK(must_load(R"({"version": 99999999999999999999})").version == 1); // parsed as float
    CHECK(must_load(R"({"version": 18446744073709551615})").version ==
          std::numeric_limits<int>::max());
    // A higher version with extra fields still loads with the v1 rules.
    const lay::Session future = must_load(R"({
        "version": 3, "theme": "dark", "window": {"width": 900, "height": 600, "dpi": 2},
        "layout": {"active_pane": 0, "pinned": [1],
                   "root": {"type": "pane", "tabs": [{"path": "/a", "tags": ["x"]}]}}})");
    CHECK(future.version == 3);
    CHECK(future.window.width == 900);
    CHECK(future.layout.tab_count() == 1);
}

TEST_CASE("json: window geometry is validated field by field", "[layout][json]") {
    const lay::Session ok = must_load(
        R"({"window": {"x": -32768, "y": 32768, "width": 32768, "height": 1, "maximized": true}})");
    CHECK(ok.window == lay::WindowGeometry{true, -32768, 32768, 32768, 1, true});

    const lay::Session bad =
        must_load(R"({"window": {"x": 10, "width": 0, "height": 40000, "maximized": "yes"}})");
    CHECK_FALSE(bad.window.has_position); // y missing
    CHECK(bad.window.x == 0);
    CHECK(bad.window.width == 1280);
    CHECK(bad.window.height == 800);
    CHECK_FALSE(bad.window.maximized);

    const lay::Session floats =
        must_load(R"({"window": {"x": 1.5, "y": 2, "width": 800.0, "height": -5}})");
    CHECK_FALSE(floats.window.has_position);
    CHECK(floats.window.width == 1280);
    CHECK(floats.window.height == 800);

    CHECK(must_load(R"({"window": {"x": 1, "y": 40000}})").window.has_position == false);
    CHECK(must_load(R"({"window": [1, 2]})").window == lay::WindowGeometry{});
    CHECK(must_load(R"({"window": {"width": 1e300}})").window.width == 1280);
    // nlohmann rejects numbers that overflow a double: not JSON.
    CHECK(lay::session_from_json(R"({"window": {"width": 1e400}})").error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("json: lenient layout repair", "[layout][json]") {
    SECTION("wrong types and unknown nodes") {
        const lay::Session s = must_load(R"({"layout": {"active_pane": "x", "root": {
            "type": "split", "axis": "horizontal", "sizes": [0.5, "big", 0.5],
            "children": [
                {"type": "pane", "active_tab": 5, "tabs": [{"path": "/a"}, "nope", {"path": "rel"}]},
                {"type": "window", "tabs": [{"path": "/ignored"}]},
                {"type": "pane", "tabs": {"path": "/notarray"}},
                {"type": "split", "axis": "diagonal", "children": [{"type": "pane", "tabs": [{"path": "/x"}]}]},
                {"type": "pane", "active_tab": -1, "tabs": [{"path": "/b", "show_hidden": 1,
                   "scroll": -4, "cursor": 7, "sort": {"key": "colour", "descending": "true",
                   "folders_first": false}}]},
                42
            ]}}})");
        REQUIRE(s.layout.pane_count() == 2);
        const lay::SplitNode& root = split_of(s.layout.root());
        CHECK(root.sizes == std::vector<double>{0.5, 0.5}); // bad sizes: equal-sized
        const std::vector<lay::PaneId> order = s.layout.pane_order();
        CHECK(tab_paths(s.layout, order[0]) == std::vector<std::string>{"/a"});
        const lay::TabState& b = s.layout.find_pane(order[1])->tabs[0].state;
        CHECK(b.path == "/b");
        CHECK_FALSE(b.show_hidden);
        CHECK_FALSE(b.scroll_rows.has_value());
        CHECK(b.cursor_name.empty());
        CHECK(b.sort.key == fs::SortKey::Name);
        CHECK_FALSE(b.sort.descending);
        CHECK_FALSE(b.sort.folders_first);
        CHECK(b.sort.natural);
        CHECK(s.layout.active_pane() == order[0]);
    }
    SECTION("NaN-like, negative and huge sizes / scroll") {
        const lay::Session s = must_load(R"({"layout": {"active_pane": 1, "root": {
            "type": "split", "axis": "vertical", "sizes": [1.7e308, 1.7e308],
            "children": [{"type": "pane", "tabs": [{"path": "/a", "scroll": -0.5}]},
                         {"type": "pane", "tabs": [{"path": "/b", "scroll": 7}]}]}}})");
        const lay::SplitNode& root = split_of(s.layout.root());
        CHECK(root.sizes == std::vector<double>{0.5, 0.5});
        const std::vector<lay::PaneId> order = s.layout.pane_order();
        CHECK_FALSE(s.layout.find_pane(order[0])->tabs[0].state.scroll_rows.has_value());
        CHECK(s.layout.find_pane(order[1])->tabs[0].state.scroll_rows == 7.0);
        CHECK(s.layout.active_pane() == order[1]);

        const lay::Session neg = must_load(R"({"layout": {"root": {
            "type": "split", "axis": "vertical", "sizes": [-1, 2],
            "children": [{"type": "pane", "tabs": [{"path": "/a"}]},
                         {"type": "pane", "tabs": [{"path": "/b"}]}]}}})");
        CHECK(split_of(neg.layout.root()).sizes == std::vector<double>{0.5, 0.5});
        const lay::Session huge = must_load(R"({"layout": {"root": {
            "type": "split", "axis": "vertical", "sizes": [1e300, 1e300, 1e-300],
            "children": [{"type": "pane", "tabs": [{"path": "/a"}]},
                         {"type": "pane", "tabs": [{"path": "/b"}]},
                         {"type": "pane", "tabs": [{"path": "/c"}]}]}}})");
        REQUIRE(split_of(huge.layout.root()).sizes.size() == 3);
        CHECK(split_of(huge.layout.root()).sizes[2] >= lay::kDefaultMinFraction - 1e-12);
    }
    SECTION("missing parts") {
        CHECK(must_load(R"({"layout": {"active_pane": 0}})").layout.tab_count() == 0);
        CHECK(must_load(R"({"layout": []})").layout.tab_count() == 0);
        CHECK(must_load(R"({"layout": {"root": {"type": "split", "axis": "horizontal"}}})")
                  .layout.tab_count() == 0);
        CHECK(must_load(R"({"layout": {"root": {"tabs": [{"path": "/a"}]}}})").layout.tab_count() ==
              0);
        const lay::Session single = must_load(
            R"({"layout": {"active_pane": 99, "root": {"type": "pane", "tabs": [{"path": "/a"}]}}})");
        CHECK(is_pane(single.layout.root()));
        CHECK(pane_of(single.layout.root()).tabs.size() == 1);
    }
    SECTION("closed tabs") {
        std::string text = R"({"closed_tabs": [{"path": "rel"}, 5, {"path": ""}, {"nopath": 1})";
        for (int i = 0; i < 30; ++i) {
            text += R"(, {"path": "/c)" + std::to_string(i) + R"("})";
        }
        text += "]}";
        const lay::Session s = must_load(text);
        REQUIRE(s.closed_tabs.size() == lay::kDefaultClosedTabCapacity);
        CHECK(s.closed_tabs.entries().front().state.path == "/c5");
        CHECK(s.closed_tabs.entries().back().state.path == "/c29");
        CHECK(must_load(R"({"closed_tabs": {"a": 1}})").closed_tabs.empty());
    }
}

TEST_CASE("json: hostile nesting does not crash and is cut at the depth cap", "[layout][json]") {
    // 5000 nested splits (alternating axes), plus deeply nested arrays in an unknown field.
    std::string text = R"({"junk": )" + std::string(20000, '[') + std::string(20000, ']') +
                       R"(, "layout": {"root": )";
    const int levels = 5000;
    for (int i = 0; i < levels; ++i) {
        text += R"({"type": "split", "axis": ")";
        text += i % 2 == 0 ? "horizontal" : "vertical";
        text += R"(", "sizes": [0.5, 0.5], "children": [{"type": "pane", "tabs": [{"path": "/p)" +
                std::to_string(i) + R"("}]}, )";
    }
    text += R"({"type": "pane", "tabs": [{"path": "/leaf"}]})";
    for (int i = 0; i < levels; ++i) {
        text += "]}";
    }
    text += "}}";
    const lay::Session s = must_load(text);
    CHECK(s.layout.pane_count() == lay::kMaxTreeDepth - 1);
    CHECK(navigator::test::depth_of(s.layout.root()) <= lay::kMaxTreeDepth);
}

TEST_CASE("json: named layout documents", "[layout][json]") {
    const lay::Session session = three_pane_session();
    const Result<std::string> text = lay::layout_to_json(session.layout);
    REQUIRE(text);
    CHECK(text->find("window") == std::string::npos);
    CHECK(text->find("closed_tabs") == std::string::npos);
    const Result<lay::Layout> back = lay::layout_from_json(*text);
    REQUIRE(back);
    CHECK(back->pane_count() == 3);
    CHECK(back->tab_count() == 5);
    CHECK(*lay::layout_to_json(*back) == *text);
    // A session document is also accepted (its "layout" member is used).
    const Result<lay::Layout> from_session = lay::layout_from_json(*lay::to_json(session));
    REQUIRE(from_session);
    CHECK(from_session->pane_count() == 3);
}

TEST_CASE("json: sidebar state round-trips and loads leniently", "[layout][json][sidebar]") {
    lay::Session session;
    session.sidebar.visible = false;
    session.sidebar.width = 301.5;
    session.sidebar.set_collapsed(lay::SidebarSection::Storage, true);
    session.sidebar.set_collapsed(lay::SidebarSection::Recents, true);
    const Result<std::string> text = lay::to_json(session);
    REQUIRE(text);
    CHECK(text->find(R"("sidebar": {
    "visible": false,
    "width": 301.5,
    "collapsed": [
      "recents",
      "storage"
    ]
  })") != std::string::npos);
    const lay::Session back = must_load(*text);
    CHECK(back.sidebar == session.sidebar);
    CHECK(*lay::to_json(back) == *text);

    // Older files (no "sidebar") and wrong types give the defaults.
    CHECK(must_load("{}").sidebar == lay::SidebarState{});
    CHECK(must_load(R"({"sidebar": [1]})").sidebar == lay::SidebarState{});
    const lay::Session odd = must_load(
        R"({"sidebar": {"visible": "no", "width": "wide", "collapsed": ["places", 3, "nope", "PLACES", "places"]}})");
    CHECK(odd.sidebar.visible);
    CHECK(odd.sidebar.width == lay::kSidebarDefaultWidth);
    CHECK(odd.sidebar.is_collapsed(lay::SidebarSection::Places));
    CHECK_FALSE(odd.sidebar.is_collapsed(lay::SidebarSection::Bookmarks));
    CHECK(must_load(R"({"sidebar": {"width": 10}})").sidebar.width == lay::kSidebarMinWidth);
    CHECK(must_load(R"({"sidebar": {"width": 1e9}})").sidebar.width == lay::kSidebarMaxWidth);
    CHECK(must_load(R"({"sidebar": {"width": 250.25}})").sidebar.width == 250.25);
    CHECK(must_load(R"({"sidebar": {"collapsed": "places"}})").sidebar == lay::SidebarState{});

    // Values the loader would repair are written repaired (save -> load -> save is stable).
    lay::Session wild;
    wild.sidebar.width = std::numeric_limits<double>::infinity();
    CHECK(lay::to_json(wild)->find(R"("width": 220,)") != std::string::npos);
    wild.sidebar.width = 5000.0;
    CHECK(lay::to_json(wild)->find(R"("width": 480,)") != std::string::npos);

    // Named layouts never carry the sidebar.
    CHECK(lay::layout_to_json(session.layout)->find("sidebar") == std::string::npos);
}
