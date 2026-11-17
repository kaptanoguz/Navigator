// Compile-only check that the Phase 3 contracts (layout tree, Layout operations, ClosedTabs,
// Session JSON and files, tab/pane/layout CommandIds, AppConfig session fields) are usable as
// documented. Nothing here is linked or run.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/layout.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

namespace navigator::contracts_check {

namespace lay = navigator::layout;

// Strong ids are distinct, trivially copyable value types; 0 is invalid.
static_assert(!std::is_convertible_v<lay::PaneId, lay::TabId>);
static_assert(!std::is_convertible_v<std::uint32_t, lay::PaneId>);
static_assert(std::is_trivially_copyable_v<lay::TabId>);
static_assert(!lay::TabId{}.valid() && lay::TabId{7}.valid());
static_assert(lay::PaneId{1} < lay::PaneId{2});
static_assert(lay::SplitId{3} == lay::SplitId{3});
static_assert(lay::axis_of(lay::SplitEdge::Left) == lay::SplitAxis::Horizontal);
static_assert(lay::axis_of(lay::SplitEdge::Bottom) == lay::SplitAxis::Vertical);
static_assert(lay::Session::kVersion == 1);
static_assert(lay::kDefaultClosedTabCapacity == 25);
static_assert(lay::WindowGeometry{} == lay::WindowGeometry{});
static_assert(fs::SortOptions{} == fs::SortOptions{});

// Values: copyable and movable (the ui snapshots a Session for a background save).
static_assert(std::is_copy_constructible_v<lay::Layout> && std::is_move_assignable_v<lay::Layout>);
static_assert(std::is_copy_constructible_v<lay::Session>);
static_assert(std::is_default_constructible_v<lay::Session>);

// Stable ids that must never change.
static_assert(static_cast<std::uint32_t>(CommandId::OpenInNewTab) == 1116);
static_assert(static_cast<std::uint32_t>(CommandId::NewTab) == 1200);
static_assert(static_cast<std::uint32_t>(CommandId::CloseTab) == 1201);
static_assert(static_cast<std::uint32_t>(CommandId::MoveTabRight) == 1209);
static_assert(static_cast<std::uint32_t>(CommandId::SplitRight) == 1220);
static_assert(static_cast<std::uint32_t>(CommandId::ClosePane) == 1224);
static_assert(static_cast<std::uint32_t>(CommandId::SaveLayout) == 1240);
static_assert(static_cast<std::uint32_t>(CommandId::DeleteLayout) == 1242);
static_assert(static_cast<std::uint32_t>(CommandId::NewWindow) == 1250);
static_assert((NewTabArg::Home & NewTabArg::None) == 0);

lay::TabState tab_at(const char* path) {
    lay::TabState state;
    state.path = path;
    state.sort.key = fs::SortKey::Modified;
    state.sort.descending = true;
    state.show_hidden = true;
    state.cursor_name = "notes.txt";
    state.scroll_rows = 12.5;
    return state;
}

std::size_t visit_tree(const lay::LayoutNode& node) {
    if (const auto* pane = std::get_if<lay::PaneNode>(&node.node)) {
        return pane->tabs.size() + pane->active_tab + pane->id.value;
    }
    const auto& split = std::get<lay::SplitNode>(node.node);
    std::size_t total = split.sizes.size() + split.id.value;
    for (const lay::LayoutNode& child : split.children) {
        total += visit_tree(child);
    }
    return total;
}

// "Three panes, drag a tab to split, close, reopen, save and restore."
std::size_t use_layout(CommandQueue& queue) {
    lay::Layout layout(tab_at("/home/u"));
    std::size_t total = 0;
    const lay::PaneId first = layout.active_pane();
    const lay::TabId home = layout.active_tab();
    const Result<lay::TabId> docs = layout.add_tab(first, tab_at("/home/u/Documents"), 1, true);
    const Result<lay::TabId> music = layout.add_tab(first, tab_at("/home/u/Music"), 99, false);
    if (!docs || !music) {
        return 0;
    }
    // Drag "Documents" to the right edge, then "Music" to the bottom edge of the new pane.
    if (Result<lay::SplitResult> right = layout.split_pane(first, lay::SplitEdge::Right, *docs)) {
        if (Result<lay::SplitResult> down =
                layout.split_pane(right->pane, lay::SplitEdge::Bottom, *music)) {
            total += down->tab == *music ? 1U : 0U;
        }
    }
    // New pane from a fresh tab (pane.split_right).
    if (Result<lay::SplitResult> fresh =
            layout.split_pane(first, lay::SplitEdge::Right, tab_at("/tmp"))) {
        total += fresh->pane.value;
    }
    total += layout.pane_count() + layout.tab_count() + visit_tree(layout.root());
    for (const lay::PaneId pane : layout.pane_order()) {
        total += layout.find_pane(pane) != nullptr ? 1U : 0U;
    }
    total += layout.neighbor_pane(first, 1).value + layout.neighbor_tab(home, -1).value;
    if (const std::optional<lay::TabLocation> where = layout.find_tab(home)) {
        total += where->index + where->pane.value;
    }
    if (lay::TabState* state = layout.tab_state(home)) {
        state->cursor_name = "Desktop";
    }
    if (const auto* split = std::get_if<lay::SplitNode>(&layout.root().node)) {
        (void)layout.move_splitter(split->id, 0, 0.3);
        (void)layout.move_splitter(split->id, 0, 0.3, 120.0 / 1280.0);
        (void)layout.equalize(split->id);
        total += layout.find_split(split->id) != nullptr ? 1U : 0U;
    }
    if (Result<void> moved = layout.move_tab(home, layout.neighbor_pane(first, 1), 0); !moved) {
        total += moved.error().code == ErrorCode::NotFound ? 1U : 0U;
    }
    (void)layout.activate_tab(home);
    (void)layout.activate_pane(first);
    if (Result<void> valid = layout.validate(); !valid) {
        total += valid.error().message.size();
    }

    lay::ClosedTabs closed;
    if (Result<lay::TabState> gone = layout.close_tab(home)) {
        closed.push(lay::ClosedTab{std::move(*gone), first, 0});
    }
    if (Result<std::vector<lay::TabState>> left = layout.close_tabs_left_of(*docs)) {
        for (lay::TabState& state : *left) {
            closed.push(lay::ClosedTab{std::move(state), lay::PaneId{}, 0});
        }
    }
    if (Result<std::vector<lay::TabState>> pane_tabs =
            layout.close_pane(layout.neighbor_pane(first, 1))) {
        total += pane_tabs->size();
    }
    if (std::optional<lay::ClosedTab> reopen = closed.pop()) {
        const lay::PaneId target =
            layout.find_pane(reopen->pane) != nullptr ? reopen->pane : layout.active_pane();
        (void)layout.add_tab(target, std::move(reopen->state), reopen->index, true);
    }
    if (Result<lay::ClosedTab> picked = closed.take(0)) {
        total += picked->index;
    }
    for (const lay::ClosedTab& entry : closed.entries()) {
        total += entry.state.path.native().size();
    }
    total += closed.size() + closed.capacity() + (closed.empty() ? 1U : 0U);
    closed.clear();

    // Tab commands target a tab by id through Command::arg.
    queue.enqueue(CommandId::CloseTab, layout.active_tab().value);
    queue.enqueue(CommandId::NewTab, NewTabArg::Home);
    queue.enqueue(CommandId::SplitDown, layout.active_pane().value);
    return total;
}

std::size_t use_session(const Path& config_root) {
    std::size_t total = 0;
    lay::Session session;
    session.window.has_position = true;
    session.window.x = -1920;
    session.window.maximized = true;
    session.layout = lay::Layout(tab_at("/srv"));
    session.closed_tabs.push(lay::ClosedTab{tab_at("/opt"), lay::PaneId{}, 0});

    if (Result<std::string> text = lay::to_json(session)) {
        if (Result<lay::Session> back = lay::session_from_json(*text)) {
            total += back->version == lay::Session::kVersion ? 1U : 0U;
            total += back->window == session.window ? 1U : 0U;
            total += back->layout.tab_count();
        }
    }
    if (Result<lay::Session> bad = lay::session_from_json("[1, 2]"); !bad) {
        total += bad.error().code == ErrorCode::InvalidArgument ? 1U : 0U;
    }

    const Path file = lay::session_file(config_root);
    if (Result<void> saved = lay::save_session(file, session); !saved) {
        total += saved.error().message.size();
    }
    if (Result<lay::Session> loaded = lay::load_session(file); !loaded) {
        total += loaded.error().code == ErrorCode::NotFound ? 1U : 0U;
    }

    const Path dir = lay::layouts_dir(config_root);
    total += lay::is_valid_layout_name("Çalışma Alanı") ? 1U : 0U;
    total += lay::is_valid_layout_name("../x") ? 0U : 1U;
    (void)lay::save_named_layout(dir, "three panes", session.layout);
    if (Result<std::vector<std::string>> names = lay::list_named_layouts(dir)) {
        total += names->size();
    }
    if (Result<lay::Layout> named = lay::load_named_layout(dir, "three panes")) {
        total += named->pane_count();
    }
    (void)lay::delete_named_layout(dir, "three panes");
    if (Result<std::string> doc = lay::layout_to_json(session.layout)) {
        total += lay::layout_from_json(*doc).has_value() ? 1U : 0U;
    }

    // Rebuild from an arbitrary (possibly invalid) tree, as the loader does.
    lay::SplitNode split;
    split.axis = lay::SplitAxis::Vertical;
    split.sizes = {0.7, 0.3};
    split.children.push_back(lay::LayoutNode{lay::PaneNode{{}, {lay::Tab{{}, tab_at("/a")}}, 0}});
    split.children.push_back(lay::LayoutNode{lay::PaneNode{}});
    const lay::Layout rebuilt = lay::Layout::from_tree(lay::LayoutNode{std::move(split)}, 1);
    total += rebuilt.pane_count();
    return total + lay::kConfigDirName.size();
}

AppConfig phase3_config() {
    AppConfig config;
    config.config_dir = "/tmp/navigator-test-config";
    config.restore_session = false;
    config.restore_window_geometry = false;
    config.save_session = false;
    return config;
}

} // namespace navigator::contracts_check
