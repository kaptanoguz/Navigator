// navigator::layout tree model: construction, every edit operation, collapse/flatten rules,
// activation rules, from_tree() repair, ClosedTabs, and a seeded random-operation property test.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <navigator/layout.hpp>

#include "layout_test_util.hpp"

using namespace navigator;
using navigator::test::depth_of;
using navigator::test::is_pane;
using navigator::test::location_of;
using navigator::test::pane_of;
using navigator::test::same_layout;
using navigator::test::split_of;
using navigator::test::tab_at;
using navigator::test::tab_paths;
namespace lay = navigator::layout;

namespace {

void require_valid(const lay::Layout& layout) {
    const Result<void> valid = layout.validate();
    INFO((valid ? std::string() : valid.error().message));
    REQUIRE(valid.has_value());
}

// Layout with one pane holding tabs /t0 .. /t<n-1>; the first tab is active.
lay::Layout with_tabs(std::size_t n) {
    lay::Layout layout(tab_at("/t0"));
    for (std::size_t i = 1; i < n; ++i) {
        REQUIRE(layout.add_tab(layout.active_pane(), tab_at("/t" + std::to_string(i)), i, false));
    }
    return layout;
}

lay::TabId tab_id(const lay::Layout& layout, lay::PaneId pane, std::size_t index) {
    return layout.find_pane(pane)->tabs.at(index).id;
}

} // namespace

TEST_CASE("layout: default construction and first tab", "[layout]") {
    const lay::Layout empty;
    require_valid(empty);
    CHECK(empty.pane_count() == 1);
    CHECK(empty.tab_count() == 0);
    CHECK(empty.active_pane().valid());
    CHECK_FALSE(empty.active_tab().valid());

    const lay::Layout one(tab_at("/home"));
    require_valid(one);
    CHECK(one.tab_count() == 1);
    CHECK(one.active_tab().valid());
    REQUIRE(one.tab_state(one.active_tab()) != nullptr);
    CHECK(one.tab_state(one.active_tab())->path == "/home");

    CHECK(lay::Layout(tab_at("relative/dir")).tab_count() == 0);
    CHECK(lay::Layout(tab_at("")).tab_count() == 0);
}

TEST_CASE("layout: add_tab clamps, activates and keeps the active tab", "[layout]") {
    lay::Layout layout(tab_at("/a"));
    const lay::PaneId pane = layout.active_pane();
    const lay::TabId a = layout.active_tab();

    const Result<lay::TabId> b = layout.add_tab(pane, tab_at("/b"), 99, false);
    REQUIRE(b);
    CHECK(tab_paths(layout, pane) == std::vector<std::string>{"/a", "/b"});
    CHECK(layout.active_tab() == a);

    // Inserting before the active tab keeps the same tab active.
    const Result<lay::TabId> c = layout.add_tab(pane, tab_at("/c"), 0, false);
    REQUIRE(c);
    CHECK(tab_paths(layout, pane) == std::vector<std::string>{"/c", "/a", "/b"});
    CHECK(layout.active_tab() == a);

    const Result<lay::TabId> d = layout.add_tab(pane, tab_at("/d"), 1, true);
    REQUIRE(d);
    CHECK(layout.active_tab() == *d);
    CHECK(location_of(layout, *d).index == 1);
    require_valid(layout);

    const lay::Layout before = layout;
    const Result<lay::TabId> rel = layout.add_tab(pane, tab_at("x"), 0, true);
    REQUIRE_FALSE(rel);
    CHECK(rel.error().code == ErrorCode::InvalidArgument);
    const Result<lay::TabId> missing = layout.add_tab(lay::PaneId{999}, tab_at("/x"), 0, true);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(same_layout(before, layout));

    // The empty only pane accepts a first tab, which becomes active even without activate.
    lay::Layout fresh;
    REQUIRE(fresh.add_tab(fresh.active_pane(), tab_at("/z"), 5, false));
    CHECK(fresh.active_tab().valid());
    require_valid(fresh);
}

TEST_CASE("layout: split_pane at each edge", "[layout]") {
    struct Case {
        lay::SplitEdge edge;
        lay::SplitAxis axis;
        bool new_first;
    };
    const Case cases[] = {
        {lay::SplitEdge::Left, lay::SplitAxis::Horizontal, true},
        {lay::SplitEdge::Right, lay::SplitAxis::Horizontal, false},
        {lay::SplitEdge::Top, lay::SplitAxis::Vertical, true},
        {lay::SplitEdge::Bottom, lay::SplitAxis::Vertical, false},
    };
    for (const Case& c : cases) {
        lay::Layout layout = with_tabs(2);
        const lay::PaneId target = layout.active_pane();
        const lay::TabId moved = tab_id(layout, target, 1);
        const Result<lay::SplitResult> r = layout.split_pane(target, c.edge, moved);
        REQUIRE(r);
        require_valid(layout);
        CHECK(r->tab == moved);
        CHECK(layout.active_pane() == r->pane);
        CHECK(layout.active_tab() == moved);
        CHECK(location_of(layout, moved).pane == r->pane);
        const lay::SplitNode& root = split_of(layout.root());
        CHECK(root.axis == c.axis);
        REQUIRE(root.children.size() == 2);
        CHECK(root.sizes == std::vector<double>{0.5, 0.5});
        CHECK(pane_of(root.children[c.new_first ? 0 : 1]).id == r->pane);
        CHECK(pane_of(root.children[c.new_first ? 1 : 0]).id == target);
        CHECK(tab_paths(layout, target) == std::vector<std::string>{"/t0"});
    }
}

TEST_CASE("layout: split inserts into a same-axis parent and nests otherwise", "[layout]") {
    lay::Layout layout(tab_at("/a"));
    const lay::PaneId a = layout.active_pane();
    const Result<lay::SplitResult> b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"));
    REQUIRE(b);
    // Same axis: B's half is shared by B and the new pane; three children in one split.
    const Result<lay::SplitResult> c =
        layout.split_pane(b->pane, lay::SplitEdge::Right, tab_at("/c"));
    REQUIRE(c);
    require_valid(layout);
    {
        const lay::SplitNode& root = split_of(layout.root());
        REQUIRE(root.children.size() == 3);
        CHECK(root.sizes == std::vector<double>{0.5, 0.25, 0.25});
        CHECK(layout.pane_order() == std::vector<lay::PaneId>{a, b->pane, c->pane});
    }
    // Left of B: inserted before B.
    const Result<lay::SplitResult> d =
        layout.split_pane(b->pane, lay::SplitEdge::Left, tab_at("/d"));
    REQUIRE(d);
    CHECK(layout.pane_order() == std::vector<lay::PaneId>{a, d->pane, b->pane, c->pane});
    CHECK(split_of(layout.root()).sizes == std::vector<double>{0.5, 0.125, 0.125, 0.25});

    // Other axis: A becomes a vertical split occupying A's old slot.
    const Result<lay::SplitResult> e = layout.split_pane(a, lay::SplitEdge::Bottom, tab_at("/e"));
    REQUIRE(e);
    require_valid(layout);
    const lay::SplitNode& root = split_of(layout.root());
    CHECK(root.sizes[0] == 0.5);
    const lay::SplitNode& nested = split_of(root.children[0]);
    CHECK(nested.axis == lay::SplitAxis::Vertical);
    CHECK(nested.sizes == std::vector<double>{0.5, 0.5});
    CHECK(layout.active_pane() == e->pane);
}

TEST_CASE("layout: split_pane errors leave the layout unchanged", "[layout]") {
    lay::Layout layout(tab_at("/a"));
    const lay::PaneId a = layout.active_pane();
    const lay::Layout before = layout;
    const Result<lay::SplitResult> own =
        layout.split_pane(a, lay::SplitEdge::Right, layout.active_tab());
    REQUIRE_FALSE(own);
    CHECK(own.error().code == ErrorCode::InvalidArgument);
    CHECK(layout.split_pane(lay::PaneId{77}, lay::SplitEdge::Right, layout.active_tab())
              .error()
              .code == ErrorCode::NotFound);
    CHECK(layout.split_pane(a, lay::SplitEdge::Right, lay::TabId{77}).error().code ==
          ErrorCode::NotFound);
    CHECK(layout.split_pane(a, lay::SplitEdge::Right, tab_at("rel")).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(layout.split_pane(lay::PaneId{77}, lay::SplitEdge::Right, tab_at("/x")).error().code ==
          ErrorCode::NotFound);
    CHECK(same_layout(before, layout));
}

TEST_CASE("layout: split with another pane's sole tab collapses the source", "[layout]") {
    lay::Layout layout(tab_at("/a"));
    const lay::PaneId a = layout.active_pane();
    const Result<lay::SplitResult> b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"));
    REQUIRE(b);
    // Drag B's only tab to A's bottom edge: H[V[A, N], B] -> B removed -> V[A, N].
    const Result<lay::SplitResult> n = layout.split_pane(a, lay::SplitEdge::Bottom, b->tab);
    REQUIRE(n);
    require_valid(layout);
    CHECK(layout.find_pane(b->pane) == nullptr);
    CHECK(layout.pane_count() == 2);
    const lay::SplitNode& root = split_of(layout.root());
    CHECK(root.axis == lay::SplitAxis::Vertical);
    CHECK(root.sizes == std::vector<double>{0.5, 0.5});
    CHECK(layout.active_pane() == n->pane);
    CHECK(tab_paths(layout, n->pane) == std::vector<std::string>{"/b"});
}

TEST_CASE("layout: move_tab reorders within a pane", "[layout]") {
    lay::Layout layout = with_tabs(3);
    const lay::PaneId pane = layout.active_pane();
    const lay::TabId t0 = tab_id(layout, pane, 0);
    REQUIRE(layout.move_tab(t0, pane, 2));
    CHECK(tab_paths(layout, pane) == std::vector<std::string>{"/t1", "/t2", "/t0"});
    CHECK(layout.active_tab() == t0);
    REQUIRE(layout.move_tab(t0, pane, 0));
    CHECK(tab_paths(layout, pane) == std::vector<std::string>{"/t0", "/t1", "/t2"});
    REQUIRE(layout.move_tab(t0, pane, 999)); // clamped to final_count - 1
    CHECK(tab_paths(layout, pane) == std::vector<std::string>{"/t1", "/t2", "/t0"});
    CHECK(location_of(layout, t0).index == 2);
    require_valid(layout);
}

TEST_CASE("layout: move_tab between panes and source collapse", "[layout]") {
    lay::Layout layout = with_tabs(2);
    const lay::PaneId a = layout.active_pane();
    const Result<lay::SplitResult> b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"));
    REQUIRE(b);
    const lay::TabId t1 = tab_id(layout, a, 1);
    REQUIRE(layout.move_tab(t1, b->pane, 0));
    CHECK(tab_paths(layout, b->pane) == std::vector<std::string>{"/t1", "/b"});
    CHECK(layout.active_pane() == b->pane);
    CHECK(layout.active_tab() == t1);
    require_valid(layout);

    // Moving A's last tab removes A; B fills the window.
    const lay::TabId t0 = tab_id(layout, a, 0);
    REQUIRE(layout.move_tab(t0, b->pane, 99));
    CHECK(layout.find_pane(a) == nullptr);
    CHECK(is_pane(layout.root()));
    CHECK(tab_paths(layout, b->pane) == std::vector<std::string>{"/t1", "/b", "/t0"});
    require_valid(layout);

    const lay::Layout before = layout;
    CHECK(layout.move_tab(lay::TabId{999}, b->pane, 0).error().code == ErrorCode::NotFound);
    CHECK(layout.move_tab(t0, lay::PaneId{999}, 0).error().code == ErrorCode::NotFound);
    CHECK(same_layout(before, layout));
}

TEST_CASE("layout: closing a pane's last tab collapses and transfers its size", "[layout]") {
    // H[A, B, C] with sizes 0.3 / 0.2 / 0.5.
    lay::Layout layout(tab_at("/a"));
    const lay::PaneId a = layout.active_pane();
    const lay::PaneId b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"))->pane;
    const lay::PaneId c = layout.split_pane(b, lay::SplitEdge::Right, tab_at("/c"))->pane;
    const lay::SplitId split = split_of(layout.root()).id;
    REQUIRE(layout.move_splitter(split, 0, 0.3, 0.0));
    REQUIRE(layout.move_splitter(split, 1, 0.5, 0.0));
    REQUIRE(split_of(layout.root()).sizes[0] == Catch::Approx(0.3));
    REQUIRE(split_of(layout.root()).sizes[1] == Catch::Approx(0.2));

    SECTION("first child: size goes to the right sibling") {
        const Result<lay::TabState> closed = layout.close_tab(layout.find_pane(a)->tabs[0].id);
        REQUIRE(closed);
        CHECK(closed->path == "/a");
        require_valid(layout);
        const lay::SplitNode& root = split_of(layout.root());
        CHECK(layout.pane_order() == std::vector<lay::PaneId>{b, c});
        CHECK(root.sizes[0] == Catch::Approx(0.5));
        CHECK(root.sizes[1] == Catch::Approx(0.5));
    }
    SECTION("middle child: size goes to the previous sibling") {
        REQUIRE(layout.close_tab(layout.find_pane(b)->tabs[0].id));
        require_valid(layout);
        const lay::SplitNode& root = split_of(layout.root());
        CHECK(layout.pane_order() == std::vector<lay::PaneId>{a, c});
        CHECK(root.sizes[0] == Catch::Approx(0.5));
    }
    SECTION("last child: size goes to the previous sibling") {
        REQUIRE(layout.close_tab(layout.find_pane(c)->tabs[0].id));
        const lay::SplitNode& root = split_of(layout.root());
        CHECK(root.sizes[0] == Catch::Approx(0.3));
        CHECK(root.sizes[1] == Catch::Approx(0.7));
    }
}

TEST_CASE("layout: collapse replaces single-child splits and flattens same-axis", "[layout]") {
    // H[X, V[H[A, B], C]]; closing C collapses V and flattens H[A, B] into the root.
    lay::Layout layout(tab_at("/a"));
    const lay::PaneId a = layout.active_pane();
    const lay::PaneId x = layout.split_pane(a, lay::SplitEdge::Left, tab_at("/x"))->pane;
    const lay::PaneId c = layout.split_pane(a, lay::SplitEdge::Bottom, tab_at("/c"))->pane;
    const lay::PaneId b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"))->pane;
    require_valid(layout);
    REQUIRE(depth_of(layout.root()) == 4);

    REQUIRE(layout.close_tab(layout.find_pane(c)->tabs[0].id));
    require_valid(layout);
    const lay::SplitNode& root = split_of(layout.root());
    CHECK(root.axis == lay::SplitAxis::Horizontal);
    CHECK(layout.pane_order() == std::vector<lay::PaneId>{x, a, b});
    REQUIRE(root.children.size() == 3);
    CHECK(root.sizes[0] == Catch::Approx(0.5));
    CHECK(root.sizes[1] == Catch::Approx(0.25));
    CHECK(root.sizes[2] == Catch::Approx(0.25));
    CHECK(depth_of(layout.root()) == 2);
}

TEST_CASE("layout: close_tab activation rules", "[layout]") {
    lay::Layout layout = with_tabs(4);
    const lay::PaneId pane = layout.active_pane();
    REQUIRE(layout.activate_tab(tab_id(layout, pane, 1)));

    // Closing the active tab activates its right neighbour.
    REQUIRE(layout.close_tab(tab_id(layout, pane, 1)));
    CHECK(layout.tab_state(layout.active_tab())->path == "/t2");
    // Closing an inactive tab before it keeps it active.
    REQUIRE(layout.close_tab(tab_id(layout, pane, 0)));
    CHECK(layout.tab_state(layout.active_tab())->path == "/t2");
    // Closing the active last tab activates the left neighbour.
    REQUIRE(layout.activate_tab(tab_id(layout, pane, 1)));
    REQUIRE(layout.close_tab(tab_id(layout, pane, 1)));
    CHECK(layout.tab_state(layout.active_tab())->path == "/t2");
    // The last tab of the only pane leaves an empty pane (I2 exception).
    REQUIRE(layout.close_tab(layout.active_tab()));
    CHECK(layout.tab_count() == 0);
    CHECK(layout.pane_count() == 1);
    CHECK_FALSE(layout.active_tab().valid());
    require_valid(layout);
    CHECK(layout.close_tab(lay::TabId{12345}).error().code == ErrorCode::NotFound);
}

TEST_CASE("layout: close_tabs_left_of", "[layout]") {
    SECTION("active tab among the closed ones") {
        lay::Layout layout = with_tabs(4);
        const lay::PaneId pane = layout.active_pane(); // t0 active
        const lay::TabId t2 = tab_id(layout, pane, 2);
        const Result<std::vector<lay::TabState>> closed = layout.close_tabs_left_of(t2);
        REQUIRE(closed);
        REQUIRE(closed->size() == 2);
        CHECK((*closed)[0].path == "/t0");
        CHECK((*closed)[1].path == "/t1");
        CHECK(tab_paths(layout, pane) == std::vector<std::string>{"/t2", "/t3"});
        CHECK(layout.active_tab() == t2);
        require_valid(layout);
    }
    SECTION("active tab to the right stays active") {
        lay::Layout layout = with_tabs(4);
        const lay::PaneId pane = layout.active_pane();
        const lay::TabId t3 = tab_id(layout, pane, 3);
        REQUIRE(layout.activate_tab(t3));
        REQUIRE(layout.close_tabs_left_of(tab_id(layout, pane, 1)));
        CHECK(layout.active_tab() == t3);
        CHECK(layout.find_pane(pane)->active_tab == 2);
        require_valid(layout);
    }
    SECTION("first tab: nothing closed") {
        lay::Layout layout = with_tabs(2);
        const Result<std::vector<lay::TabState>> closed =
            layout.close_tabs_left_of(tab_id(layout, layout.active_pane(), 0));
        REQUIRE(closed);
        CHECK(closed->empty());
        CHECK(layout.tab_count() == 2);
    }
    lay::Layout layout;
    CHECK(layout.close_tabs_left_of(lay::TabId{9}).error().code == ErrorCode::NotFound);
}

TEST_CASE("layout: close_pane and the active pane rule", "[layout]") {
    lay::Layout layout = with_tabs(2);
    const lay::PaneId a = layout.active_pane();
    CHECK(layout.close_pane(a).error().code == ErrorCode::InvalidArgument);
    CHECK(layout.close_pane(lay::PaneId{999}).error().code == ErrorCode::NotFound);

    const lay::PaneId b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"))->pane;
    const lay::PaneId c = layout.split_pane(b, lay::SplitEdge::Right, tab_at("/c"))->pane;

    // Active middle pane closed: the next pane (same position) becomes active.
    REQUIRE(layout.activate_pane(b));
    const Result<std::vector<lay::TabState>> gone = layout.close_pane(b);
    REQUIRE(gone);
    REQUIRE(gone->size() == 1);
    CHECK(layout.active_pane() == c);
    // Active last pane closed: the (new) last pane becomes active.
    REQUIRE(layout.close_pane(c));
    CHECK(layout.active_pane() == a);
    CHECK(is_pane(layout.root()));
    require_valid(layout);

    // Closing an inactive pane keeps the active pane.
    const lay::PaneId d = layout.split_pane(a, lay::SplitEdge::Bottom, tab_at("/d"))->pane;
    REQUIRE(layout.activate_pane(a));
    const Result<std::vector<lay::TabState>> tabs = layout.close_pane(d);
    REQUIRE(tabs);
    CHECK(layout.active_pane() == a);
    CHECK(tab_paths(layout, a) == std::vector<std::string>{"/t0", "/t1"});
}

TEST_CASE("layout: move_splitter clamps and validates", "[layout]") {
    lay::Layout layout(tab_at("/a"));
    const lay::PaneId a = layout.active_pane();
    const lay::PaneId b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"))->pane;
    REQUIRE(layout.split_pane(b, lay::SplitEdge::Right, tab_at("/c")));
    const lay::SplitId split = split_of(layout.root()).id; // 0.5 / 0.25 / 0.25
    const auto sizes = [&] { return layout.find_split(split)->sizes; };

    REQUIRE(layout.move_splitter(split, 0, 0.6));
    CHECK(sizes()[0] == Catch::Approx(0.6));
    CHECK(sizes()[1] == Catch::Approx(0.15));
    CHECK(sizes()[2] == Catch::Approx(0.25)); // untouched

    // Clamped to min_fraction on either side.
    REQUIRE(layout.move_splitter(split, 0, 0.99));
    CHECK(sizes()[1] == Catch::Approx(lay::kDefaultMinFraction));
    REQUIRE(layout.move_splitter(split, 0, -5.0, 0.1));
    CHECK(sizes()[0] == Catch::Approx(0.1));
    CHECK(sizes()[0] + sizes()[1] == Catch::Approx(0.75));

    // Splitter 1 is measured from the split start too.
    REQUIRE(layout.move_splitter(split, 1, 0.5));
    CHECK(sizes()[1] == Catch::Approx(0.4));
    CHECK(sizes()[2] == Catch::Approx(0.5));

    // Both cannot keep min_fraction: half each.
    REQUIRE(layout.move_splitter(split, 0, 0.12, 0.0));
    REQUIRE(layout.move_splitter(split, 0, 0.11, 0.0)); // sizes 0.11 / 0.39
    REQUIRE(layout.move_splitter(split, 0, 0.0, 0.3));  // combined 0.5 < 0.6
    CHECK(sizes()[0] == Catch::Approx(0.25));
    CHECK(sizes()[1] == Catch::Approx(0.25));

    // min_fraction 0 never produces a zero size.
    REQUIRE(layout.move_splitter(split, 0, -1.0, 0.0));
    CHECK(sizes()[0] > 0.0);
    require_valid(layout);

    const lay::Layout before = layout;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(layout.move_splitter(split, 2, 0.5).error().code == ErrorCode::InvalidArgument);
    CHECK(layout.move_splitter(split, 0, nan).error().code == ErrorCode::InvalidArgument);
    CHECK(layout.move_splitter(split, 0, 0.5, nan).error().code == ErrorCode::InvalidArgument);
    CHECK(layout.move_splitter(split, 0, 0.5, -0.1).error().code == ErrorCode::InvalidArgument);
    CHECK(layout.move_splitter(split, 0, 0.5, 0.5).error().code == ErrorCode::InvalidArgument);
    CHECK(layout.move_splitter(lay::SplitId{999}, 0, 0.5).error().code == ErrorCode::NotFound);
    CHECK(same_layout(before, layout));

    REQUIRE(layout.equalize(split));
    for (const double s : sizes()) {
        CHECK(s == Catch::Approx(1.0 / 3.0));
    }
    require_valid(layout);
    CHECK(layout.equalize(lay::SplitId{999}).error().code == ErrorCode::NotFound);
}

TEST_CASE("layout: neighbor_pane and neighbor_tab wrap around", "[layout]") {
    lay::Layout layout = with_tabs(3);
    const lay::PaneId a = layout.active_pane();
    const lay::PaneId b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"))->pane;
    const lay::PaneId c = layout.split_pane(b, lay::SplitEdge::Bottom, tab_at("/c"))->pane;
    CHECK(layout.pane_order() == std::vector<lay::PaneId>{a, b, c});
    CHECK(layout.neighbor_pane(a, 1) == b);
    CHECK(layout.neighbor_pane(c, 1) == a);
    CHECK(layout.neighbor_pane(a, -1) == c);
    CHECK(layout.neighbor_pane(b, 0) == b);
    CHECK(layout.neighbor_pane(a, 7) == b);
    CHECK(layout.neighbor_pane(a, std::numeric_limits<int>::min()) ==
          layout.pane_order()[static_cast<std::size_t>(((std::numeric_limits<int>::min() % 3) + 3) %
                                                       3)]);
    CHECK_FALSE(layout.neighbor_pane(lay::PaneId{999}, 1).valid());

    const lay::TabId t0 = tab_id(layout, a, 0);
    const lay::TabId t2 = tab_id(layout, a, 2);
    CHECK(layout.neighbor_tab(t0, 1) == tab_id(layout, a, 1));
    CHECK(layout.neighbor_tab(t0, -1) == t2);
    CHECK(layout.neighbor_tab(t2, 1) == t0);
    CHECK(layout.neighbor_tab(t0, 0) == t0);
    CHECK_FALSE(layout.neighbor_tab(lay::TabId{999}, 1).valid());
}

TEST_CASE("layout: activate_tab and activate_pane", "[layout]") {
    lay::Layout layout = with_tabs(2);
    const lay::PaneId a = layout.active_pane();
    const lay::PaneId b = layout.split_pane(a, lay::SplitEdge::Right, tab_at("/b"))->pane;
    const lay::TabId t1 = tab_id(layout, a, 1);
    REQUIRE(layout.activate_tab(t1));
    CHECK(layout.active_pane() == a);
    CHECK(layout.active_tab() == t1);
    REQUIRE(layout.activate_pane(b));
    CHECK(layout.active_tab() == tab_id(layout, b, 0));
    REQUIRE(layout.activate_pane(a));
    CHECK(layout.active_tab() == t1); // pane remembers its active tab
    CHECK(layout.activate_tab(lay::TabId{999}).error().code == ErrorCode::NotFound);
    CHECK(layout.activate_pane(lay::PaneId{}).error().code == ErrorCode::NotFound);

    lay::TabState* state = layout.tab_state(t1);
    REQUIRE(state != nullptr);
    state->cursor_name = "x";
    CHECK(layout.tab_state(t1)->cursor_name == "x");
    CHECK(layout.tab_state(lay::TabId{999}) == nullptr);
}

TEST_CASE("layout: depth cap refuses nesting deeper than kMaxTreeDepth", "[layout]") {
    lay::Layout layout(tab_at("/root"));
    lay::PaneId pane = layout.active_pane();
    std::size_t splits = 0;
    for (;;) {
        const lay::SplitEdge edge =
            splits % 2 == 0 ? lay::SplitEdge::Right : lay::SplitEdge::Bottom;
        const lay::Layout before = layout;
        const Result<lay::SplitResult> r = layout.split_pane(pane, edge, tab_at("/p"));
        if (!r) {
            CHECK(r.error().code == ErrorCode::InvalidArgument);
            CHECK(same_layout(before, layout));
            break;
        }
        require_valid(layout);
        pane = r->pane;
        ++splits;
        REQUIRE(splits < 100);
    }
    CHECK(depth_of(layout.root()) == lay::kMaxTreeDepth);
    CHECK(splits == lay::kMaxTreeDepth - 1);
}

TEST_CASE("layout: ids are fresh, distinct and never reused", "[layout]") {
    lay::Layout layout = with_tabs(2);
    const lay::TabId t1 = tab_id(layout, layout.active_pane(), 1);
    REQUIRE(layout.close_tab(t1));
    const Result<lay::TabId> again = layout.add_tab(layout.active_pane(), tab_at("/n"), 1, true);
    REQUIRE(again);
    CHECK(*again != t1);
    CHECK(again->value > t1.value);
}

TEST_CASE("layout: from_tree repairs arbitrary trees", "[layout]") {
    using lay::LayoutNode;
    using lay::PaneNode;
    using lay::SplitNode;
    const auto pane = [](const std::vector<std::string>& paths, std::size_t active = 0) {
        PaneNode p;
        for (const std::string& path : paths) {
            p.tabs.push_back(lay::Tab{lay::TabId{}, tab_at(path)});
        }
        p.active_tab = active;
        return LayoutNode{std::move(p)};
    };
    const auto split = [](lay::SplitAxis axis, std::vector<double> sizes,
                          std::vector<LayoutNode> children) {
        SplitNode s;
        s.axis = axis;
        s.sizes = std::move(sizes);
        s.children = std::move(children);
        return LayoutNode{std::move(s)};
    };
    constexpr auto H = lay::SplitAxis::Horizontal;
    constexpr auto V = lay::SplitAxis::Vertical;

    SECTION("invalid tabs and empty panes dropped, single child collapses") {
        std::vector<LayoutNode> kids;
        kids.push_back(pane({"rel", "/ok", ""}, 1));
        kids.push_back(pane({"also-relative"}));
        const lay::Layout layout = lay::Layout::from_tree(split(H, {0.5, 0.5}, std::move(kids)));
        require_valid(layout);
        REQUIRE(is_pane(layout.root()));
        CHECK(tab_paths(layout, layout.active_pane()) == std::vector<std::string>{"/ok"});
        CHECK(layout.tab_state(layout.active_tab())->path == "/ok");
    }
    SECTION("active_tab out of range becomes 0; surviving active tab is kept") {
        const lay::Layout out = lay::Layout::from_tree(pane({"/a", "/b"}, 7));
        CHECK(pane_of(out.root()).active_tab == 0);
        const lay::Layout kept = lay::Layout::from_tree(pane({"x", "/a", "/b"}, 2));
        CHECK(kept.tab_state(kept.active_tab())->path == "/b");
    }
    SECTION("bad sizes equalize; good sizes rescale and get the minimum fraction") {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<std::vector<double>> bad = {
            {0.5},       {0.5, 0.5, 0.5, 0.5},
            {nan, 1, 1}, {-1, 1, 1},
            {0, 1, 1},   {std::numeric_limits<double>::infinity(), 1, 1}};
        for (const std::vector<double>& sizes : bad) {
            std::vector<LayoutNode> kids;
            kids.push_back(pane({"/a"}));
            kids.push_back(pane({"/b"}));
            kids.push_back(pane({"/c"}));
            const lay::Layout layout = lay::Layout::from_tree(split(H, sizes, std::move(kids)));
            require_valid(layout);
            for (const double s : split_of(layout.root()).sizes) {
                CHECK(s == Catch::Approx(1.0 / 3.0));
            }
        }
        std::vector<LayoutNode> kids;
        kids.push_back(pane({"/a"}));
        kids.push_back(pane({"/b"}));
        kids.push_back(pane({"/c"}));
        const lay::Layout layout =
            lay::Layout::from_tree(split(V, {6.0, 1e-9, 2.0}, std::move(kids)));
        require_valid(layout);
        const std::vector<double>& sizes = split_of(layout.root()).sizes;
        CHECK(sizes[1] == Catch::Approx(lay::kDefaultMinFraction));
        CHECK(sizes[0] == Catch::Approx(0.95 * 0.75));
        CHECK(sizes[2] == Catch::Approx(0.95 * 0.25));
        CHECK(split_of(layout.root()).axis == V);
    }
    SECTION("dropped children take their size with them; same-axis nesting flattens") {
        std::vector<LayoutNode> inner;
        inner.push_back(pane({"/b"}));
        inner.push_back(pane({"/c"}));
        std::vector<LayoutNode> kids;
        kids.push_back(pane({"/a"}));
        kids.push_back(pane({}));
        kids.push_back(split(H, {1, 3}, std::move(inner)));
        const lay::Layout layout =
            lay::Layout::from_tree(split(H, {0.5, 0.25, 0.25}, std::move(kids)), 2);
        require_valid(layout);
        const lay::SplitNode& root = split_of(layout.root());
        REQUIRE(root.children.size() == 3);
        CHECK(root.sizes[0] == Catch::Approx(2.0 / 3.0));
        CHECK(root.sizes[1] == Catch::Approx(1.0 / 12.0));
        CHECK(root.sizes[2] == Catch::Approx(0.25));
        CHECK(layout.pane_order().size() == 3);
        CHECK(layout.active_pane() == layout.pane_order()[2]);
    }
    SECTION("single-child split whose child has the parent's axis flattens") {
        std::vector<LayoutNode> leaf;
        leaf.push_back(pane({"/b"}));
        leaf.push_back(pane({"/c"}));
        std::vector<LayoutNode> mid;
        mid.push_back(split(H, {0.5, 0.5}, std::move(leaf)));
        mid.push_back(pane({}));
        std::vector<LayoutNode> kids;
        kids.push_back(pane({"/a"}));
        kids.push_back(split(V, {0.5, 0.5}, std::move(mid)));
        const lay::Layout layout = lay::Layout::from_tree(split(H, {0.5, 0.5}, std::move(kids)));
        require_valid(layout);
        CHECK(split_of(layout.root()).children.size() == 3);
        CHECK(depth_of(layout.root()) == 2);
    }
    SECTION("subtrees deeper than kMaxTreeDepth are dropped") {
        // Alternating axes, 40 levels deep; each level holds a pane and the next level.
        LayoutNode node = pane({"/deepest"});
        for (std::size_t level = 40; level-- > 1;) {
            std::vector<LayoutNode> kids;
            kids.push_back(pane({"/level" + std::to_string(level)}));
            kids.push_back(std::move(node));
            node = split(level % 2 == 0 ? H : V, {0.5, 0.5}, std::move(kids));
        }
        const lay::Layout layout = lay::Layout::from_tree(node, 1000);
        require_valid(layout);
        CHECK(depth_of(layout.root()) <= lay::kMaxTreeDepth);
        CHECK(layout.active_pane() == layout.pane_order()[0]);
        for (const lay::PaneId p : layout.pane_order()) {
            CHECK(tab_paths(layout, p) != std::vector<std::string>{"/deepest"});
        }
    }
    SECTION("nothing usable gives Layout()") {
        std::vector<LayoutNode> kids;
        kids.push_back(pane({}));
        kids.push_back(pane({"rel"}));
        const lay::Layout layout = lay::Layout::from_tree(split(H, {}, std::move(kids)));
        require_valid(layout);
        CHECK(layout.pane_count() == 1);
        CHECK(layout.tab_count() == 0);
        const lay::Layout from_empty_split = lay::Layout::from_tree(LayoutNode{SplitNode{}});
        CHECK(from_empty_split.tab_count() == 0);
        SplitNode bad_axis;
        bad_axis.axis = static_cast<lay::SplitAxis>(9);
        bad_axis.sizes = {0.5, 0.5};
        bad_axis.children.push_back(pane({"/a"}));
        bad_axis.children.push_back(pane({"/b"}));
        const lay::Layout from_bad_axis = lay::Layout::from_tree(LayoutNode{std::move(bad_axis)});
        CHECK(from_bad_axis.tab_count() == 0);
    }
    SECTION("ids in the input are ignored") {
        PaneNode p;
        p.id = lay::PaneId{5};
        p.tabs.push_back(lay::Tab{lay::TabId{5}, tab_at("/a")});
        p.tabs.push_back(lay::Tab{lay::TabId{5}, tab_at("/b")});
        const lay::Layout layout = lay::Layout::from_tree(LayoutNode{std::move(p)});
        require_valid(layout);
        CHECK(layout.tab_count() == 2);
    }
}

TEST_CASE("layout: random operation sequences keep the invariants", "[layout][property]") {
    std::mt19937 rng(20261117U);
    const auto pick = [&](std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    };
    const auto all_tabs = [](const lay::Layout& layout) {
        std::vector<lay::TabId> tabs;
        for (const lay::PaneId p : layout.pane_order()) {
            for (const lay::Tab& t : layout.find_pane(p)->tabs) {
                tabs.push_back(t.id);
            }
        }
        return tabs;
    };
    const auto all_splits = [](const lay::Layout& layout) {
        std::vector<lay::SplitId> ids;
        std::vector<const lay::LayoutNode*> stack{&layout.root()};
        while (!stack.empty()) {
            const lay::LayoutNode* node = stack.back();
            stack.pop_back();
            if (const auto* s = std::get_if<lay::SplitNode>(&node->node)) {
                ids.push_back(s->id);
                for (const lay::LayoutNode& child : s->children) {
                    stack.push_back(&child);
                }
            }
        }
        return ids;
    };

    lay::Layout layout(tab_at("/start"));
    std::size_t failures = 0;
    std::size_t successes = 0;
    std::size_t max_panes = 0;
    for (int step = 0; step < 6000; ++step) {
        const lay::Layout before = layout;
        const std::vector<lay::PaneId> panes = layout.pane_order();
        const std::vector<lay::TabId> tabs = all_tabs(layout);
        const std::vector<lay::SplitId> splits = all_splits(layout);
        // Mostly real ids, sometimes a bogus one to exercise NotFound.
        const lay::PaneId pane = pick(10) == 0 ? lay::PaneId{999999} : panes[pick(panes.size())];
        const lay::TabId tab =
            tabs.empty() || pick(10) == 0 ? lay::TabId{999999} : tabs[pick(tabs.size())];
        const auto edge = static_cast<lay::SplitEdge>(pick(4));
        const std::size_t index = pick(6);
        const lay::TabState state =
            tab_at(pick(12) == 0 ? "relative" : "/d" + std::to_string(step));
        bool succeeded = false;
        switch (pick(11)) {
        case 0: succeeded = layout.add_tab(pane, state, index, pick(2) == 0).has_value(); break;
        case 1:
            if (layout.tab_count() > 1) {
                succeeded = layout.close_tab(tab).has_value();
            }
            break;
        case 2: succeeded = layout.close_tabs_left_of(tab).has_value(); break;
        case 3: succeeded = layout.close_pane(pane).has_value(); break;
        case 4: succeeded = layout.move_tab(tab, pane, index).has_value(); break;
        case 5: succeeded = layout.split_pane(pane, edge, tab).has_value(); break;
        case 6: succeeded = layout.split_pane(pane, edge, state).has_value(); break;
        case 7: succeeded = layout.activate_tab(tab).has_value(); break;
        case 8: succeeded = layout.activate_pane(pane).has_value(); break;
        case 9:
            if (!splits.empty()) {
                const lay::SplitId split = splits[pick(splits.size())];
                const double position = std::uniform_real_distribution<double>(-0.2, 1.2)(rng);
                const double min_fraction =
                    std::uniform_real_distribution<double>(-0.05, 0.55)(rng);
                succeeded =
                    layout.move_splitter(split, pick(4), position, min_fraction).has_value();
            }
            break;
        default:
            if (!splits.empty()) {
                succeeded = layout.equalize(splits[pick(splits.size())]).has_value();
            }
            break;
        }
        const Result<void> valid = layout.validate();
        INFO("step " << step << ": " << (valid ? std::string() : valid.error().message));
        REQUIRE(valid.has_value());
        if (succeeded) {
            ++successes;
        } else {
            ++failures;
            REQUIRE(same_layout(before, layout));
        }
        max_panes = std::max(max_panes, layout.pane_count());
        // Keep the tree from growing without bound; the JSON round trip must also hold.
        if (layout.pane_count() > 24) {
            while (layout.pane_count() > 3) {
                REQUIRE(layout.close_pane(layout.pane_order().back()));
                require_valid(layout);
            }
        }
        if (step % 500 == 0) {
            const Result<std::string> text = lay::layout_to_json(layout);
            REQUIRE(text);
            const Result<lay::Layout> back = lay::layout_from_json(*text);
            REQUIRE(back);
            require_valid(*back);
            CHECK(back->pane_count() == layout.pane_count());
            CHECK(back->tab_count() == layout.tab_count());
        }
    }
    CHECK(successes > 1000);
    CHECK(failures > 100);
    CHECK(max_panes > 5);
}

TEST_CASE("closed tabs: capacity, order, pop and take", "[layout]") {
    lay::ClosedTabs closed(3);
    CHECK(closed.capacity() == 3);
    CHECK(closed.empty());
    CHECK_FALSE(closed.pop().has_value());
    for (int i = 0; i < 5; ++i) {
        closed.push(lay::ClosedTab{tab_at("/c" + std::to_string(i)), lay::PaneId{1},
                                   static_cast<std::size_t>(i)});
    }
    REQUIRE(closed.size() == 3);
    CHECK(closed.entries()[0].state.path == "/c2");
    CHECK(closed.entries()[2].state.path == "/c4");

    const Result<lay::ClosedTab> mid = closed.take(1);
    REQUIRE(mid);
    CHECK(mid->state.path == "/c3");
    CHECK(mid->index == 3);
    CHECK(closed.take(2).error().code == ErrorCode::NotFound);
    const std::optional<lay::ClosedTab> newest = closed.pop();
    REQUIRE(newest);
    CHECK((newest.has_value() && newest->state.path == "/c4"));
    CHECK(closed.size() == 1);
    closed.clear();
    CHECK(closed.empty());

    CHECK(lay::ClosedTabs(0).capacity() == 1);
    lay::ClosedTabs defaults;
    CHECK(defaults.capacity() == lay::kDefaultClosedTabCapacity);
    for (int i = 0; i < 40; ++i) {
        defaults.push(lay::ClosedTab{tab_at("/x" + std::to_string(i)), {}, 0});
    }
    CHECK(defaults.size() == lay::kDefaultClosedTabCapacity);
    CHECK(defaults.entries().front().state.path == "/x15");
}
