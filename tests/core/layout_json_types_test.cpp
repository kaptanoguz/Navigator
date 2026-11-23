// navigator::layout JSON v1, Phase 5 "hidden_types" (TabState::type_filter): round trip in
// sessions, closed tabs and named layouts; absent when nothing is hidden; deterministic sorted
// output; lenient loading with the search::normalize(TypeFilter) rules.
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/layout.hpp>
#include <navigator/search.hpp>

#include "layout_test_util.hpp"

using namespace navigator;
using navigator::test::tab_at;
namespace lay = navigator::layout;

namespace {

const lay::TabState& first_tab(const lay::Layout& layout) {
    const auto& pane = std::get<lay::PaneNode>(layout.root().node);
    REQUIRE(!pane.tabs.empty());
    return pane.tabs.front().state;
}

// The type filter of the single tab in {"layout": {"root": pane[{path, hidden_types}]}}.
search::TypeFilter load_types(const std::string& hidden_types) {
    const std::string text = R"({"layout": {"root": {"type": "pane", "tabs": [{"path": "/a", )"
                             R"("hidden_types": )" +
                             hidden_types + "}]}}}";
    INFO(text);
    const Result<lay::Session> session = lay::session_from_json(text);
    REQUIRE(session);
    REQUIRE(session->layout.tab_count() == 1);
    return first_tab(session->layout).type_filter;
}

search::TypeFilter filter(bool folders, bool no_ext, std::vector<std::string> exts) {
    search::TypeFilter f;
    f.hide_folders = folders;
    f.hide_no_extension = no_ext;
    f.hidden_extensions = std::move(exts);
    return f;
}

} // namespace

TEST_CASE("json types: absent when nothing is hidden", "[layout][json][types]") {
    lay::Session session;
    session.layout = lay::Layout(tab_at("/home/u"));
    session.closed_tabs.push(lay::ClosedTab{tab_at("/opt"), {}, 0});
    const Result<std::string> text = lay::to_json(session);
    REQUIRE(text);
    CHECK(text->find("hidden_types") == std::string::npos);
    const Result<std::string> named = lay::layout_to_json(session.layout);
    REQUIRE(named);
    CHECK(named->find("hidden_types") == std::string::npos);

    // Only invalid extensions: the normalized filter is inactive, so nothing is written.
    lay::TabState junk = tab_at("/j");
    junk.type_filter.hidden_extensions = {"", "a/b", std::string("x\0y", 3)};
    const Result<std::string> junk_text = lay::layout_to_json(lay::Layout(junk));
    REQUIRE(junk_text);
    CHECK(junk_text->find("hidden_types") == std::string::npos);
}

TEST_CASE("json types: golden tab text, keys in order, sorted extensions",
          "[layout][json][types]") {
    lay::TabState tab = tab_at("/home/u");
    tab.type_filter = filter(true, true, {"TXT", "jpeg", "İso", "JPEG", "a"});
    const Result<std::string> text = lay::layout_to_json(lay::Layout(tab));
    REQUIRE(text);
    const std::string expected = R"(            "case_insensitive": true
          },
          "hidden_types": {
            "folders": true,
            "no_extension": true,
            "extensions": [
              "a",
              "iso",
              "jpeg",
              "txt"
            ]
          }
        }
      ]
    }
  }
}
)";
    CHECK(text->ends_with(expected));

    // Only the flags that are set are written.
    lay::TabState only_folders = tab_at("/f");
    only_folders.type_filter.hide_folders = true;
    const Result<std::string> f_text = lay::layout_to_json(lay::Layout(only_folders));
    REQUIRE(f_text);
    CHECK(f_text->find(R"("hidden_types": {
            "folders": true
          })") != std::string::npos);

    lay::TabState only_ext = tab_at("/e");
    only_ext.type_filter.hidden_extensions = {"png"};
    const Result<std::string> e_text = lay::layout_to_json(lay::Layout(only_ext));
    REQUIRE(e_text);
    CHECK(e_text->find("\"folders\"") == std::string::npos);
    CHECK(e_text->find("\"no_extension\"") == std::string::npos);
    CHECK(e_text->find(R"("extensions": [
              "png"
            ])") != std::string::npos);

    // Deterministic: input order does not matter.
    lay::TabState shuffled = tab;
    shuffled.type_filter.hidden_extensions = {"a", "JPEG", "txt", "iso", "jpeg"};
    CHECK(*lay::layout_to_json(lay::Layout(shuffled)) == *text);
}

TEST_CASE("json types: round trip in sessions, closed tabs and named layouts",
          "[layout][json][types]") {
    lay::TabState home = tab_at("/home/u/Çalışma");
    home.type_filter = filter(true, false, {"md", "pdf", "ğüş", std::string("\xff\xfe")});
    lay::TabState other = tab_at("/srv");
    other.type_filter = filter(false, true, {});
    lay::TabState plain = tab_at("/tmp");

    lay::Session session;
    session.layout = lay::Layout(home);
    REQUIRE(session.layout.add_tab(session.layout.active_pane(), other, 1, false));
    REQUIRE(session.layout.add_tab(session.layout.active_pane(), plain, 2, false));
    session.closed_tabs.push(lay::ClosedTab{home, {}, 0});

    const Result<std::string> text = lay::to_json(session);
    REQUIRE(text);
    CHECK(text->find(R"("hex": "fffe")") != std::string::npos); // non-UTF-8 extension
    const Result<lay::Session> loaded = lay::session_from_json(*text);
    REQUIRE(loaded);
    const auto& tabs = std::get<lay::PaneNode>(loaded->layout.root().node).tabs;
    REQUIRE(tabs.size() == 3);
    search::TypeFilter expected_home = home.type_filter;
    expected_home.hidden_extensions = {"md", "pdf", "ğüş", std::string("\xff\xfe")};
    CHECK(tabs[0].state.type_filter == expected_home);
    CHECK(tabs[1].state.type_filter == other.type_filter);
    CHECK(tabs[2].state.type_filter == search::TypeFilter{});
    REQUIRE(loaded->closed_tabs.entries().size() == 1);
    CHECK(loaded->closed_tabs.entries().front().state.type_filter == expected_home);
    CHECK(*lay::to_json(*loaded) == *text); // save -> load -> save is stable

    const Result<std::string> named = lay::layout_to_json(session.layout);
    REQUIRE(named);
    const Result<lay::Layout> named_loaded = lay::layout_from_json(*named);
    REQUIRE(named_loaded);
    CHECK(first_tab(*named_loaded).type_filter == expected_home);
    CHECK(*lay::layout_to_json(*named_loaded) == *named);
}

TEST_CASE("json types: lenient loading", "[layout][json][types]") {
    // Missing / not an object -> nothing hidden.
    CHECK(load_types("null") == search::TypeFilter{});
    CHECK(load_types("[\"txt\"]") == search::TypeFilter{});
    CHECK(load_types("\"txt\"") == search::TypeFilter{});
    CHECK(load_types("{}") == search::TypeFilter{});

    // Wrong-typed flags -> false; unknown fields ignored.
    CHECK(load_types(R"({"folders": "yes", "no_extension": 1, "future": [1, 2]})") ==
          search::TypeFilter{});
    CHECK(load_types(R"({"folders": true, "no_extension": false, "x": {}})") ==
          filter(true, false, {}));

    // "extensions" not an array -> none; non-<bytes> items ignored; invalid ones dropped;
    // folded, sorted, unique.
    CHECK(load_types(R"({"extensions": "txt"})") == search::TypeFilter{});
    CHECK(load_types(R"({"extensions": {"txt": true}})") == search::TypeFilter{});
    CHECK(load_types(R"({"no_extension": true, "extensions": [5, null, true, ["a"], {"hex": "zz"},
                         "", "a/b", "x\u0000y", "TXT", "txt", "PNG", "İSO", {"hex": "4A5047"},
                         {"hex": "ff"}]})") ==
          filter(false, true, {"iso", "jpg", "png", "txt", std::string("\xff")}));
}

TEST_CASE("json types: tabs without the field (older files) load unchanged",
          "[layout][json][types]") {
    const Result<lay::Session> session = lay::session_from_json(
        R"({"version": 1, "layout": {"root": {"type": "pane", "tabs": [{"path": "/a"}]}}})");
    REQUIRE(session);
    CHECK(first_tab(session->layout).type_filter == search::TypeFilter{});
    CHECK(!first_tab(session->layout).type_filter.active());
}
