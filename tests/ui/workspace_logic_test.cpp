// Phase 3 pure logic: pane geometry, tab strip widths/overflow, drop-zone hit testing and the
// drag state machine (drop -> resulting layout operation), Search Tabs filtering, session save
// debounce. Headless, no files.
#include <cmath>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <navigator/i18n.hpp>
#include <navigator/layout.hpp>

#include "workspace/drag_drop.hpp"
#include "workspace/pane_geometry.hpp"
#include "workspace/session_writer.hpp"
#include "workspace/tab_search.hpp"
#include "workspace/tab_strip.hpp"

using namespace navigator;
using namespace navigator::ui;
using Catch::Matchers::WithinAbs;
using layout::Layout;
using layout::PaneId;
using layout::SplitEdge;
using layout::TabId;
using layout::TabState;

namespace {

TabState at(const char* path) {
    TabState s;
    s.path = path;
    return s;
}

// [A | B] with B split vertically into [B / C]: three panes.
struct ThreePanes {
    Layout layout{at("/a")};
    PaneId a;
    PaneId b;
    PaneId c;
    ThreePanes() {
        a = layout.active_pane();
        b = layout.split_pane(a, SplitEdge::Right, at("/b"))->pane;
        c = layout.split_pane(b, SplitEdge::Bottom, at("/c"))->pane;
    }
};

std::vector<std::string> paths(const Layout& l, PaneId pane) {
    std::vector<std::string> out;
    if (const layout::PaneNode* p = l.find_pane(pane)) {
        for (const layout::Tab& t : p->tabs) {
            out.push_back(t.state.path.string());
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Pane geometry
// ---------------------------------------------------------------------------------------------

TEST_CASE("Pane rectangles tile the window without gaps or overlap", "[workspace][geometry]") {
    ThreePanes t;
    const PaneGeometry g = compute_pane_geometry(t.layout, RectF{0, 0, 1000, 600}, 0.0F, 8.0F);
    REQUIRE(g.panes.size() == 3);
    const RectF a = g.find(t.a)->rect;
    const RectF b = g.find(t.b)->rect;
    const RectF c = g.find(t.c)->rect;
    CHECK(a == RectF{0, 0, 500, 600});
    CHECK(b == RectF{500, 0, 500, 300});
    CHECK(c == RectF{500, 300, 500, 300});
    // Reading order is the layout's pane order.
    CHECK(g.panes[0].pane == t.a);
    CHECK(g.panes[2].pane == t.c);
    REQUIRE(g.splitters.size() == 2);
    // The deeper (vertical) splitter comes first so it wins where hit areas cross.
    CHECK(g.splitters[0].axis == layout::SplitAxis::Vertical);
    CHECK(g.splitter_at(500.0F, 100.0F)->axis == layout::SplitAxis::Horizontal);
    CHECK(g.splitter_at(700.0F, 302.0F)->axis == layout::SplitAxis::Vertical);
    CHECK(g.splitter_at(200.0F, 100.0F) == nullptr);
    CHECK(g.pane_at(800.0F, 450.0F)->pane == t.c);
}

TEST_CASE("Gaps are carved out between children and splitter positions round-trip",
          "[workspace][geometry]") {
    Layout l(at("/a"));
    const PaneId a = l.active_pane();
    (void)l.split_pane(a, SplitEdge::Right, at("/b"));
    const PaneGeometry g = compute_pane_geometry(l, RectF{10, 0, 1010, 400}, 10.0F, 8.0F);
    REQUIRE(g.panes.size() == 2);
    CHECK(g.panes[0].rect.w == 500.0F);
    CHECK(g.panes[1].rect.x == 520.0F);
    CHECK(g.panes[1].rect.right() == 1020.0F);
    const SplitterRect& s = g.splitters.front();
    CHECK(s.slot == RectF{510, 0, 10, 400});
    CHECK(s.hit.w == 10.0F); // never thinner than the gap
    // Pointer in the middle of the slot = the current boundary.
    CHECK_THAT(splitter_position(s, 515.0F), WithinAbs(0.5, 1e-6));
    CHECK_THAT(splitter_position(s, 315.0F), WithinAbs(0.3, 1e-6));
    CHECK_THAT(min_fraction(s, 250.0F), WithinAbs(0.25, 1e-9));
    CHECK_THAT(min_fraction(s, 5000.0F), WithinAbs(0.49, 1e-9));
    // Dragging there and recomputing puts the boundary under the pointer.
    REQUIRE(l.move_splitter(s.split, 0, splitter_position(s, 315.0F), 0.05));
    const PaneGeometry g2 = compute_pane_geometry(l, RectF{10, 0, 1010, 400}, 10.0F, 8.0F);
    CHECK(g2.splitters.front().slot.center_x() == 315.0F);
}

// ---------------------------------------------------------------------------------------------
// Tab strip
// ---------------------------------------------------------------------------------------------

TEST_CASE("Tabs take their natural width when they fit", "[workspace][strip]") {
    TabStripInput in;
    in.natural = {120.0F, 60.5F, 400.0F};
    in.available = 1000.0F;
    in.min_width = 90.0F;
    in.max_width = 220.0F;
    in.gap = 2.0F;
    const TabStripLayout l = layout_tab_strip(in);
    CHECK(l.w == std::vector<float>{120.0F, 90.0F, 220.0F}); // clamped to [min, max]
    CHECK(l.x == std::vector<float>{0.0F, 122.0F, 214.0F});
    CHECK(l.content == 434.0F);
    CHECK_FALSE(l.overflow);
    CHECK(l.scroll == 0.0F);
}

TEST_CASE("Tabs shrink the widest first, then overflow and scroll", "[workspace][strip]") {
    TabStripInput in;
    in.natural = {100.0F, 200.0F, 200.0F};
    in.min_width = 90.0F;
    in.max_width = 220.0F;
    in.gap = 0.0F;
    in.available = 400.0F;
    TabStripLayout l = layout_tab_strip(in);
    CHECK_FALSE(l.overflow);
    CHECK(l.w == std::vector<float>{100.0F, 150.0F, 150.0F}); // short label keeps its width
    in.available = 300.0F;
    l = layout_tab_strip(in);
    CHECK(l.w == std::vector<float>{100.0F, 100.0F, 100.0F});
    in.available = 200.0F; // 3 * 90 > 200
    in.scroll = 500.0F;
    l = layout_tab_strip(in);
    CHECK(l.overflow);
    CHECK(l.w == std::vector<float>{90.0F, 90.0F, 90.0F});
    CHECK(l.scroll == 70.0F); // clamped to content - available
    CHECK(scroll_to_show(l, 0, 200.0F, l.scroll) == 0.0F);
    CHECK(scroll_to_show(l, 2, 200.0F, 0.0F) == 70.0F);
    CHECK(scroll_to_show(l, 1, 200.0F, 0.0F) == 0.0F); // already visible
}

TEST_CASE("Insertion index counts tab centres left of the pointer", "[workspace][strip]") {
    TabStripInput in;
    in.natural = {100.0F, 100.0F, 100.0F};
    in.available = 1000.0F;
    in.min_width = 50.0F;
    in.max_width = 200.0F;
    const TabStripLayout l = layout_tab_strip(in);
    CHECK(insertion_index(l, -5.0F) == 0);
    CHECK(insertion_index(l, 49.0F) == 0);
    CHECK(insertion_index(l, 51.0F) == 1);
    CHECK(insertion_index(l, 260.0F) == 3);
    CHECK(layout_tab_strip(TabStripInput{}).x.empty());
}

// ---------------------------------------------------------------------------------------------
// Drop zones and the drag state machine
// ---------------------------------------------------------------------------------------------

namespace {
PaneDropArea area(PaneId pane, RectF rect) {
    PaneDropArea a;
    a.pane = pane;
    a.rect = rect;
    a.strip = RectF{rect.x, rect.y, rect.w, 30.0F};
    a.tab_center = {rect.x + 60.0F, rect.x + 180.0F};
    return a;
}
} // namespace

TEST_CASE("Drop hit testing: strip, edge bands, centre, outside", "[workspace][drag]") {
    const PaneId p1{1};
    const PaneId p2{2};
    const std::vector<PaneDropArea> areas{area(p1, {0, 0, 400, 430}), area(p2, {400, 0, 400, 430})};
    const DropZoneMetrics m{0.25F, 160.0F};
    // Body of p1: y 30..430 (400 tall), bands 100 px.
    DropTarget t = hit_test_drop(areas, 100.0F, 10.0F, m);
    CHECK(t.kind == DropKind::TabBar);
    CHECK(t.pane == p1);
    CHECK(t.index == 1);
    CHECK(hit_test_drop(areas, 590.0F, 10.0F, m).index == 2);
    t = hit_test_drop(areas, 380.0F, 230.0F, m);
    CHECK(t.kind == DropKind::Edge);
    CHECK(t.edge == SplitEdge::Right);
    CHECK(t.pane == p1);
    CHECK(hit_test_drop(areas, 420.0F, 230.0F, m).edge == SplitEdge::Left);
    CHECK(hit_test_drop(areas, 200.0F, 40.0F, m).edge == SplitEdge::Top);
    CHECK(hit_test_drop(areas, 200.0F, 420.0F, m).edge == SplitEdge::Bottom);
    // Corner: the band the pointer is relatively deeper in loses (nearest edge wins).
    CHECK(hit_test_drop(areas, 5.0F, 100.0F, m).edge == SplitEdge::Left);
    CHECK(hit_test_drop(areas, 200.0F, 230.0F, m).kind == DropKind::Centre);
    CHECK(hit_test_drop(areas, 900.0F, 230.0F, m).kind == DropKind::None);
    // Preview: half of the body on the edge's side, the whole body for Centre.
    DropTarget right{DropKind::Edge, p1, 0, SplitEdge::Right};
    CHECK(drop_preview(areas[0], right) == RectF{200, 30, 200, 400});
    DropTarget bottom{DropKind::Edge, p1, 0, SplitEdge::Bottom};
    CHECK(drop_preview(areas[0], bottom) == RectF{0, 230, 400, 200});
    CHECK(drop_preview(areas[0], DropTarget{DropKind::Centre, p1, 0, SplitEdge::Left}) ==
          RectF{0, 30, 400, 400});
    CHECK(drop_preview(areas[0], DropTarget{}).w == 0.0F);
}

TEST_CASE("Drag state machine: click, drag, cancel", "[workspace][drag]") {
    TabDrag d;
    CHECK(d.state() == TabDrag::State::Idle);
    d.press(TabId{7}, PaneId{1}, 10.0F, 10.0F);
    CHECK_FALSE(d.move(12.0F, 12.0F, 4.0F)); // within the threshold
    TabDrag::Outcome o = d.release();
    CHECK(o.kind == TabDrag::Outcome::Kind::Click);
    CHECK(o.tab == TabId{7});
    CHECK(d.state() == TabDrag::State::Idle);

    d.press(TabId{7}, PaneId{1}, 10.0F, 10.0F);
    CHECK(d.move(20.0F, 10.0F, 4.0F));
    CHECK(d.dragging());
    const DropTarget target{DropKind::Edge, PaneId{2}, 0, SplitEdge::Right};
    d.hover(target);
    o = d.release();
    CHECK(o.kind == TabDrag::Outcome::Kind::Drop);
    CHECK(o.target == target);

    d.press(TabId{7}, PaneId{1}, 0.0F, 0.0F);
    (void)d.move(50.0F, 0.0F, 4.0F);
    d.cancel(); // Escape / focus lost
    CHECK(d.release().kind == TabDrag::Outcome::Kind::None);
}

TEST_CASE("A drop becomes one layout operation", "[workspace][drag]") {
    SECTION("Edge -> split_pane") {
        Layout l(at("/a"));
        const PaneId a = l.active_pane();
        const TabId t2 = *l.add_tab(a, at("/b"), 1, false);
        CHECK(apply_drop(l, t2, DropTarget{DropKind::Edge, a, 0, SplitEdge::Right}));
        CHECK(l.pane_count() == 2);
        CHECK(l.find_tab(t2).value_or(layout::TabLocation{layout::PaneId{}, 99}).pane ==
              l.active_pane());
        CHECK(l.pane_order().back() == l.active_pane());
        CHECK(l.validate());
    }
    SECTION("Edge with a pane's only tab on its own edge is a no-op") {
        Layout l(at("/a"));
        const PaneId a = l.active_pane();
        const TabId t1 = l.active_tab();
        CHECK_FALSE(apply_drop(l, t1, DropTarget{DropKind::Edge, a, 0, SplitEdge::Left}));
        CHECK(l.pane_count() == 1);
    }
    SECTION("TabBar in the same pane reorders, counting the tab itself") {
        Layout l(at("/a"));
        const PaneId a = l.active_pane();
        const TabId t1 = l.active_tab();
        (void)l.add_tab(a, at("/b"), 1, false);
        (void)l.add_tab(a, at("/c"), 2, false);
        // Drop /a after /b: insertion index 2 (centres of /a and /b are left of the pointer).
        CHECK(apply_drop(l, t1, DropTarget{DropKind::TabBar, a, 2, SplitEdge::Left}));
        CHECK(paths(l, a) == std::vector<std::string>{"/b", "/a", "/c"});
        CHECK(apply_drop(l, t1, DropTarget{DropKind::TabBar, a, 0, SplitEdge::Left}));
        CHECK(paths(l, a) == std::vector<std::string>{"/a", "/b", "/c"});
        CHECK(apply_drop(l, t1, DropTarget{DropKind::TabBar, a, 3, SplitEdge::Left}));
        CHECK(paths(l, a) == std::vector<std::string>{"/b", "/c", "/a"});
    }
    SECTION("TabBar of another pane moves it there; Centre appends; source collapses") {
        ThreePanes t;
        const TabId tb = t.layout.find_pane(t.b)->tabs[0].id;
        CHECK(apply_drop(t.layout, tb, DropTarget{DropKind::TabBar, t.a, 0, SplitEdge::Left}));
        CHECK(paths(t.layout, t.a) == std::vector<std::string>{"/b", "/a"});
        CHECK(t.layout.pane_count() == 2); // the emptied pane collapsed
        const TabId tc = t.layout.find_pane(t.c)->tabs[0].id;
        const TabId ta = t.layout.find_pane(t.a)->tabs[1].id;
        CHECK(apply_drop(t.layout, ta, DropTarget{DropKind::Centre, t.c, 0, SplitEdge::Left}));
        CHECK(paths(t.layout, t.c) == std::vector<std::string>{"/c", "/a"});
        CHECK_FALSE(
            apply_drop(t.layout, tc, DropTarget{DropKind::Centre, t.c, 0, SplitEdge::Left}));
        CHECK_FALSE(apply_drop(t.layout, tc, DropTarget{}));
        CHECK_FALSE(apply_drop(t.layout, TabId{999},
                               DropTarget{DropKind::Centre, t.a, 0, SplitEdge::Left}));
        CHECK(t.layout.validate());
    }
}

// ---------------------------------------------------------------------------------------------
// Search Tabs
// ---------------------------------------------------------------------------------------------

TEST_CASE("Search Tabs lists open tabs by pane, then closed tabs newest first",
          "[workspace][search]") {
    ThreePanes t;
    (void)t.layout.add_tab(t.a, at("/home/u/Çalışma Alanı"), 1, true);
    layout::ClosedTabs closed;
    closed.push(layout::ClosedTab{at("/old"), PaneId{}, 0});
    closed.push(layout::ClosedTab{at("/home/u/Downloads"), PaneId{}, 0});
    const Path home("/home/u");
    const std::vector<TabSearchItem> items = collect_tab_search_items(t.layout, closed, &home);
    REQUIRE(items.size() == 6);
    CHECK(items[0].title == "a");
    CHECK(items[1].title == "Çalışma Alanı");
    CHECK(items[1].detail == "~/Çalışma Alanı");
    CHECK(items[1].active);
    CHECK(items[1].pane_number == 1);
    CHECK(items[3].pane_number == 3);
    CHECK(items[4].source == TabSearchItem::Source::Closed);
    CHECK(items[4].title == "Downloads");
    CHECK(items[4].closed_index == 1);
    CHECK(items[5].closed_index == 0);

    CHECK(filter_tab_search(items, "").size() == 6);
    CHECK(filter_tab_search(items, "   ").size() == 6);
    // Case and Turkish dotless i fold away.
    CHECK(filter_tab_search(items, "CALISMA").empty()); // ç is not c (no diacritic folding)
    CHECK(filter_tab_search(items, "ÇALIŞMA") == std::vector<std::size_t>{1});
    CHECK(filter_tab_search(items, "çalişma") == std::vector<std::size_t>{1});
    // Every word must match title or path.
    CHECK(filter_tab_search(items, "~ down") == std::vector<std::size_t>{4});
    CHECK(filter_tab_search(items, "zzz").empty());
    // Title-prefix matches first within each group.
    const std::vector<std::size_t> o = filter_tab_search(items, "o");
    REQUIRE_FALSE(o.empty());
    CHECK(o.front() != 4);
    const std::vector<std::size_t> d = filter_tab_search(items, "d");
    // Group order is kept: open before closed.
    for (std::size_t i = 1; i < d.size(); ++i) {
        CHECK((items[d[i - 1]].source <= items[d[i]].source));
    }
}

TEST_CASE("Tab titles", "[workspace][search]") {
    navigator::set_language(Language::English);
    CHECK(tab_title(Path("/")) == "File System");
    CHECK(tab_title(Path("/usr/share/")) == "share");
    CHECK(tab_title(Path("/a/b\xFF")) == "b\xEF\xBF\xBD");
    const Path home("/home/u");
    CHECK(tab_detail(Path("/home/u"), &home) == "~");
    CHECK(tab_detail(Path("/home/user2"), &home) == "/home/user2");
    CHECK(tab_detail(Path("/etc"), nullptr) == "/etc");
}

// ---------------------------------------------------------------------------------------------
// Session save debounce
// ---------------------------------------------------------------------------------------------

TEST_CASE("Session saves are debounced 2 s after the last change", "[workspace][session]") {
    SaveScheduler s;
    CHECK_FALSE(s.pending());
    CHECK_FALSE(s.remaining(0.0).has_value());
    s.mark(1000.0);
    CHECK_FALSE(s.due(2999.0));
    s.mark(2500.0); // another change pushes the deadline
    CHECK_FALSE(s.due(4000.0));
    CHECK_THAT(s.remaining(4000.0).value_or(-1.0), WithinAbs(500.0, 1e-9));
    CHECK(s.due(4500.0));
    CHECK(s.remaining(9000.0).value_or(-1.0) == 0.0);
    s.taken();
    CHECK_FALSE(s.pending());
}
