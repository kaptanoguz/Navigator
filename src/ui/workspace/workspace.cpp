// src/ui/workspace/workspace.cpp — see workspace.hpp.
#include "workspace/workspace.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <set>
#include <system_error>
#include <utility>

#include <navigator/fs.hpp>

namespace navigator::ui {

using layout::PaneId;
using layout::TabId;
using layout::TabState;

Path normalize_dir(const Path& dir) {
    std::error_code ec;
    Path p = dir.is_absolute() ? dir : std::filesystem::absolute(dir, ec);
    if (ec) {
        p = dir;
    }
    p = p.lexically_normal();
    if (!p.has_filename() && p != p.root_path()) {
        p = p.parent_path();
    }
    return p;
}

Workspace::Workspace(Options options) : options_(std::move(options)) {}

Workspace::~Workspace() = default;

std::unique_ptr<BrowserModel> Workspace::make_model() const {
    BrowserModel::Options o = options_.model;
    o.start_suspended = true;
    return std::make_unique<BrowserModel>(std::move(o));
}

Path Workspace::fallback_dir() const {
    if (!start_dir_.empty()) {
        return start_dir_;
    }
    return options_.home.empty() ? Path("/") : options_.home;
}

void Workspace::start(std::optional<layout::Session> restored, const Path& start_path) {
    if (restored) {
        session_ = std::move(*restored);
    }
    if (!start_path.empty()) {
        start_dir_ = normalize_dir(start_path);
    }
    if (session_.layout.tab_count() == 0) {
        TabState first;
        first.path = normalize_dir(fallback_dir());
        session_.layout = layout::Layout(first);
    } else if (!start_path.empty()) {
        const PaneId pane = session_.layout.active_pane();
        const layout::PaneNode* node = session_.layout.find_pane(pane);
        const std::size_t at = node != nullptr ? node->active_tab + 1 : 0;
        TabState s;
        s.path = start_dir_;
        (void)session_.layout.add_tab(pane, s, at, true);
    }
    models_.clear();
    ++epoch_;
    reconcile();
    changed();
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

BrowserModel* Workspace::model(TabId tab) noexcept {
    const auto it = models_.find(tab.value);
    return it == models_.end() ? nullptr : it->second.get();
}

const BrowserModel* Workspace::model(TabId tab) const noexcept {
    const auto it = models_.find(tab.value);
    return it == models_.end() ? nullptr : it->second.get();
}

BrowserModel& Workspace::active_model() {
    BrowserModel* m = model(active_tab());
    if (m == nullptr) {
        reconcile(); // never expected: start() and every edit reconcile
        m = model(active_tab());
    }
    return *m;
}

std::vector<TabId> Workspace::visible_tabs() const {
    std::vector<TabId> out;
    for (const PaneId pane : layout().pane_order()) {
        const layout::PaneNode* p = layout().find_pane(pane);
        if (p != nullptr && p->active_tab < p->tabs.size()) {
            out.push_back(p->tabs[p->active_tab].id);
        }
    }
    return out;
}

bool Workspace::settled() const {
    for (const TabId t : visible_tabs()) {
        const BrowserModel* m = model(t);
        if (m == nullptr || !m->loaded() || m->busy()) {
            return false;
        }
    }
    return true;
}

bool Workspace::searching() const {
    for (const TabId t : visible_tabs()) {
        const BrowserModel* m = model(t);
        if (m != nullptr && m->searching()) {
            return true;
        }
    }
    return false;
}

void Workspace::cancel_walks() noexcept {
    for (auto& [id, m] : models_) {
        m->cancel_for_quit();
    }
}

bool Workspace::frame_start() {
    bool any = false;
    for (auto& [id, m] : models_) {
        if (!m->suspended()) {
            any = m->frame_start() || any;
        }
    }
    return any;
}

bool Workspace::sync() {
    bool significant = false;
    for (auto& [id, m] : models_) {
        TabState* cur = session_.layout.tab_state(TabId{id});
        if (cur == nullptr) {
            continue;
        }
        TabState next = m->tab_state(options_.row_dp);
        if (next.path.empty()) {
            next.path = cur->path;
        }
        if (next.path != cur->path || !(next.sort == cur->sort) ||
            next.show_hidden != cur->show_hidden || !(next.type_filter == cur->type_filter)) {
            significant = true;
        }
        *cur = std::move(next);
    }
    if (significant) {
        changed();
    }
    return significant;
}

void Workspace::reconcile() {
    std::set<std::uint32_t> alive;
    const layout::Layout& l = session_.layout;
    for (const PaneId pane : l.pane_order()) {
        const layout::PaneNode* p = l.find_pane(pane);
        if (p == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < p->tabs.size(); ++i) {
            const layout::Tab& t = p->tabs[i];
            alive.insert(t.id.value);
            auto it = models_.find(t.id.value);
            if (it == models_.end()) {
                auto m = make_model();
                m->restore(t.state, options_.row_dp, record_tabs_.erase(t.id.value) > 0);
                it = models_.emplace(t.id.value, std::move(m)).first;
            }
            if (i == p->active_tab) {
                it->second->resume();
            } else {
                it->second->suspend();
            }
        }
    }
    for (auto it = models_.begin(); it != models_.end();) {
        if (alive.contains(it->first)) {
            ++it;
        } else {
            it = models_.erase(it);
        }
    }
}

TabState Workspace::new_tab_state(PaneId pane, bool home) const {
    TabState s;
    if (home) {
        s.path = options_.home.empty() ? Path("/") : options_.home;
        return s;
    }
    const layout::PaneNode* p = layout().find_pane(pane);
    if (p != nullptr && p->active_tab < p->tabs.size()) {
        const layout::Tab& t = p->tabs[p->active_tab];
        const BrowserModel* m = model(t.id);
        const TabState src = m != nullptr ? m->tab_state(options_.row_dp) : t.state;
        s.path = src.path.empty() ? t.state.path : src.path;
        s.sort = src.sort;
        s.show_hidden = src.show_hidden;
        s.type_filter = src.type_filter; // a view setting like show_hidden (layout.hpp)
    }
    if (s.path.empty()) {
        s.path = fallback_dir();
    }
    return s;
}

TabId Workspace::target_tab(std::uint64_t arg) const {
    if (arg == 0) {
        return active_tab();
    }
    if (arg > std::numeric_limits<std::uint32_t>::max()) {
        return TabId{};
    }
    const TabId t{static_cast<std::uint32_t>(arg)};
    return layout().find_tab(t) ? t : TabId{};
}

PaneId Workspace::target_pane(std::uint64_t arg) const {
    if (arg == 0) {
        return active_pane();
    }
    if (arg > std::numeric_limits<std::uint32_t>::max()) {
        return PaneId{};
    }
    const PaneId p{static_cast<std::uint32_t>(arg)};
    return layout().find_pane(p) != nullptr ? p : PaneId{};
}

void Workspace::push_closed(TabState state, PaneId pane, std::size_t index) {
    session_.closed_tabs.push(layout::ClosedTab{std::move(state), pane, index});
    changed();
}

void Workspace::reopen(layout::ClosedTab closed) {
    layout::Layout& l = session_.layout;
    if (closed.pane.valid() && l.find_pane(closed.pane) != nullptr) {
        if (l.add_tab(closed.pane, closed.state, closed.index, true)) {
            return;
        }
    }
    const PaneId pane = l.active_pane();
    const layout::PaneNode* p = l.find_pane(pane);
    const std::size_t at = p != nullptr ? p->active_tab + 1 : 0;
    (void)l.add_tab(pane, std::move(closed.state), at, true);
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

bool Workspace::execute(const Command& command) {
    layout::Layout& l = session_.layout;
    const auto edit = [this](auto&& fn) {
        (void)sync();
        if (fn()) {
            changed();
        }
        reconcile();
    };
    switch (command.id) {
    case CommandId::NewTab: {
        const bool home = (command.arg & NewTabArg::Home) != 0U;
        edit([&] {
            const PaneId pane = l.active_pane();
            const layout::PaneNode* p = l.find_pane(pane);
            const std::size_t at = p != nullptr ? p->active_tab + 1 : 0;
            const Result<TabId> added = l.add_tab(pane, new_tab_state(pane, home), at, true);
            if (added) {
                record_tabs_.insert(added->value);
            }
            return added.has_value();
        });
        return true;
    }
    case CommandId::CloseTab: {
        const TabId tab = target_tab(command.arg);
        if (!tab.valid() || l.tab_count() <= 1) {
            return true; // the last tab of the last pane is never closed
        }
        edit([&] {
            const std::optional<layout::TabLocation> loc = l.find_tab(tab);
            Result<TabState> s = l.close_tab(tab);
            if (!s || !loc) {
                return false;
            }
            push_closed(std::move(*s), loc->pane, loc->index);
            return true;
        });
        return true;
    }
    case CommandId::DuplicateTab: {
        const TabId tab = target_tab(command.arg);
        if (!tab.valid()) {
            return true;
        }
        (void)sync();
        const std::optional<layout::TabLocation> loc = l.find_tab(tab);
        const TabState* state = l.tab_state(tab);
        if (!loc || state == nullptr) {
            return true;
        }
        Result<TabId> added = l.add_tab(loc->pane, *state, loc->index + 1, true);
        if (added) {
            auto m = make_model();
            if (const BrowserModel* src = model(tab); src != nullptr) {
                m->copy_from(*src);
            } else {
                m->restore(*state, options_.row_dp);
            }
            models_[added->value] = std::move(m);
            changed();
        }
        reconcile();
        return true;
    }
    case CommandId::CloseLeftTabs: {
        const TabId tab = target_tab(command.arg);
        if (!tab.valid()) {
            return true;
        }
        edit([&] {
            const std::optional<layout::TabLocation> loc = l.find_tab(tab);
            Result<std::vector<TabState>> closed = l.close_tabs_left_of(tab);
            if (!closed || !loc || closed->empty()) {
                return false;
            }
            // Right to left: reopen (newest first) then rebuilds the bar in its old order.
            for (std::size_t i = closed->size(); i > 0; --i) {
                push_closed(std::move((*closed)[i - 1]), loc->pane, i - 1);
            }
            return true;
        });
        return true;
    }
    case CommandId::ReopenClosedTab: {
        edit([&] {
            std::optional<layout::ClosedTab> c = session_.closed_tabs.pop();
            if (!c) {
                return false;
            }
            reopen(std::move(*c));
            return true;
        });
        return true;
    }
    case CommandId::NextTab:
    case CommandId::PreviousTab: {
        const TabId next = l.neighbor_tab(active_tab(), command.id == CommandId::NextTab ? 1 : -1);
        if (next.valid() && next != active_tab()) {
            activate_tab(next);
        }
        return true;
    }
    case CommandId::MoveTabLeft:
    case CommandId::MoveTabRight: {
        const TabId tab = target_tab(command.arg);
        const std::optional<layout::TabLocation> loc = tab.valid() ? l.find_tab(tab) : std::nullopt;
        if (!loc) {
            return true;
        }
        const layout::PaneNode* p = l.find_pane(loc->pane);
        const bool left = command.id == CommandId::MoveTabLeft;
        if (p == nullptr || (left && loc->index == 0) ||
            (!left && loc->index + 1 >= p->tabs.size())) {
            return true; // no wrap
        }
        edit([&] {
            return l.move_tab(tab, loc->pane, left ? loc->index - 1 : loc->index + 1).has_value();
        });
        return true;
    }
    case CommandId::SplitRight:
    case CommandId::SplitDown: {
        const PaneId pane = target_pane(command.arg);
        if (!pane.valid()) {
            return true;
        }
        edit([&] {
            const layout::SplitEdge edge = command.id == CommandId::SplitRight
                                               ? layout::SplitEdge::Right
                                               : layout::SplitEdge::Bottom;
            const Result<layout::SplitResult> r =
                l.split_pane(pane, edge, new_tab_state(pane, false));
            if (r) {
                record_tabs_.insert(r->tab.value);
            }
            return r.has_value();
        });
        return true;
    }
    case CommandId::FocusNextPane:
    case CommandId::FocusPreviousPane: {
        const PaneId next =
            l.neighbor_pane(active_pane(), command.id == CommandId::FocusNextPane ? 1 : -1);
        if (next.valid() && next != active_pane()) {
            activate_pane(next);
        }
        return true;
    }
    case CommandId::ClosePane: {
        const PaneId pane = target_pane(command.arg);
        if (!pane.valid() || l.pane_count() <= 1) {
            return true;
        }
        edit([&] {
            Result<std::vector<TabState>> closed = l.close_pane(pane);
            if (!closed) {
                return false;
            }
            // Right to left: the pane is gone, so each reopen lands after the tab reopened
            // before it, and the first tab comes back first.
            for (std::size_t i = closed->size(); i > 0; --i) {
                push_closed(std::move((*closed)[i - 1]), pane, i - 1);
            }
            return true;
        });
        return true;
    }
    case CommandId::OpenInNewTab: {
        BrowserModel& m = active_model();
        const fs::DirEntry* e = m.cursor_entry();
        if (e == nullptr) {
            return true;
        }
        if (!fs::is_directory_like(*e)) {
            m.activate();
            return true;
        }
        (void)open_tab(m.row_path(m.cursor()), false); // flattened rows live below the folder
        return true;
    }
    case CommandId::NewWindow:
        return true; // reserved: one window until the multi-window phase
    // Phase 5: shell level (filter focus, types popup, volume table, index service; ui::App).
    case CommandId::FocusFilter:
    case CommandId::FlattenVolume:
    case CommandId::ToggleTypesPopup:
    case CommandId::RebuildIndex: return false;
    // Phase 6: the Inspector (shell level; ui::App).
    case CommandId::Inspect:
    case CommandId::ToggleInspectorLock:
    case CommandId::InspectorZoomFit:
    case CommandId::InspectorZoom1to1:
    case CommandId::InspectorPlay:
    case CommandId::CycleInspectorPlacement:
    case CommandId::ToggleInspectorGrid:
    case CommandId::ToggleInspectorMinimap: return false;
    // Phase 5: the active tab's view.
    case CommandId::ClearFilter:
    case CommandId::ToggleFlatten:
    case CommandId::ToggleFuzzy:
    case CommandId::CancelSearch: return active_model().execute(command);
    // Phase 4: shell level (sidebar, GoTo, address bar, bookmarks, clipboard; ui::App).
    case CommandId::FocusAddressBar:
    case CommandId::GoTo:
    case CommandId::ToggleSidebar:
    case CommandId::ToggleViewLock:
    case CommandId::FocusSidebar:
    case CommandId::ToggleBookmark:
    case CommandId::RenameBookmark:
    case CommandId::MoveBookmarkUp:
    case CommandId::MoveBookmarkDown:
    case CommandId::ClearRecents:
    case CommandId::ToggleRecentsTracking:
    case CommandId::CopyPath:
    case CommandId::SearchTabs:
    case CommandId::SaveLayout:
    case CommandId::LoadLayout:
    case CommandId::DeleteLayout:
    case CommandId::None:
    case CommandId::Quit:
    case CommandId::ToggleFpsOverlay:
    case CommandId::ToggleTheme: return false;
    case CommandId::ToggleHidden:
    case CommandId::NavigateBack:
    case CommandId::NavigateForward:
    case CommandId::NavigateUp:
    case CommandId::Refresh:
    case CommandId::Activate:
    case CommandId::SelectAll:
    case CommandId::ClearSelection:
    case CommandId::CursorUp:
    case CommandId::CursorDown:
    case CommandId::CursorPageUp:
    case CommandId::CursorPageDown:
    case CommandId::CursorHome:
    case CommandId::CursorEnd: return active_model().execute(command);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Phase 4: sidebar, GoTo, address bar
// ---------------------------------------------------------------------------------------------

void Workspace::navigate_active(const Path& dir, std::string cursor_name) {
    active_model().navigate_to(dir, std::move(cursor_name));
}

bool Workspace::open_tab(const Path& dir, bool activate) {
    layout::Layout& l = session_.layout;
    BrowserModel& m = active_model();
    TabState s;
    s.path = normalize_dir(dir);
    s.sort = m.sort();
    s.show_hidden = m.show_hidden();
    s.type_filter = m.type_filter();
    bool done = false;
    (void)sync();
    const PaneId pane = l.active_pane();
    const layout::PaneNode* p = l.find_pane(pane);
    const std::size_t at = p != nullptr ? p->active_tab + 1 : 0;
    if (const Result<TabId> added = l.add_tab(pane, s, at, activate); added) {
        record_tabs_.insert(added->value);
        changed();
        done = true;
    }
    reconcile();
    return done;
}

std::vector<Path> Workspace::take_visits() {
    std::vector<Path> out;
    for (auto& [id, m] : models_) {
        for (Path& v : m->take_visits()) {
            out.push_back(std::move(v));
        }
    }
    return out;
}

void Workspace::set_sidebar(const layout::SidebarState& state) {
    layout::SidebarState next = state;
    next.width = std::clamp(next.width, layout::kSidebarMinWidth, layout::kSidebarMaxWidth);
    if (!(next == session_.sidebar)) {
        session_.sidebar = next;
        changed();
    }
}

// ---------------------------------------------------------------------------------------------
// Pointer intents
// ---------------------------------------------------------------------------------------------

void Workspace::activate_pane(PaneId pane) {
    if (pane == active_pane()) {
        return;
    }
    (void)sync();
    if (session_.layout.activate_pane(pane)) {
        changed();
    }
    reconcile();
}

void Workspace::activate_tab(TabId tab) {
    (void)sync();
    const TabId before = active_tab();
    if (session_.layout.activate_tab(tab) && before != tab) {
        changed();
    }
    reconcile();
}

bool Workspace::drop(TabId tab, const DropTarget& target) {
    (void)sync();
    const bool done = apply_drop(session_.layout, tab, target);
    if (done) {
        changed();
    }
    reconcile();
    return done;
}

void Workspace::move_splitter(layout::SplitId split, std::size_t splitter, double position,
                              double min_fraction) {
    if (session_.layout.move_splitter(split, splitter, position, min_fraction)) {
        changed();
    }
}

void Workspace::equalize(layout::SplitId split) {
    if (session_.layout.equalize(split)) {
        changed();
    }
}

void Workspace::reopen_closed_at(std::size_t closed_index) {
    (void)sync();
    Result<layout::ClosedTab> c = session_.closed_tabs.take(closed_index);
    if (c) {
        reopen(std::move(*c));
        changed();
    }
    reconcile();
}

// ---------------------------------------------------------------------------------------------
// Named layouts
// ---------------------------------------------------------------------------------------------

Result<std::vector<std::string>> Workspace::list_layouts() const {
    if (options_.config_root.empty()) {
        return fail(ErrorCode::Unsupported, "no config directory");
    }
    return layout::list_named_layouts(layout::layouts_dir(options_.config_root));
}

bool Workspace::layout_exists(std::string_view name) const {
    const Result<std::vector<std::string>> names = list_layouts();
    return names && std::find(names->begin(), names->end(), name) != names->end();
}

Result<void> Workspace::save_layout(std::string_view name) {
    if (options_.config_root.empty()) {
        return fail(ErrorCode::Unsupported, "no config directory");
    }
    (void)sync();
    return layout::save_named_layout(layout::layouts_dir(options_.config_root), name,
                                     session_.layout);
}

Result<void> Workspace::load_layout(std::string_view name) {
    if (options_.config_root.empty()) {
        return fail(ErrorCode::Unsupported, "no config directory");
    }
    Result<layout::Layout> loaded =
        layout::load_named_layout(layout::layouts_dir(options_.config_root), name);
    if (!loaded) {
        return fail(loaded.error().code, loaded.error().message);
    }
    (void)sync();
    // The replaced tabs go to the closed-tabs list (their pane ids die with the layout), last
    // first, so repeated reopens bring them back in reading order.
    const std::vector<PaneId> order = session_.layout.pane_order();
    for (const PaneId pane : std::views::reverse(order)) {
        const layout::PaneNode* p = session_.layout.find_pane(pane);
        if (p == nullptr) {
            continue;
        }
        for (std::size_t i = p->tabs.size(); i > 0; --i) {
            session_.closed_tabs.push(layout::ClosedTab{p->tabs[i - 1].state, PaneId{}, i - 1});
        }
    }
    session_.layout = std::move(*loaded);
    if (session_.layout.tab_count() == 0) {
        TabState first;
        first.path = normalize_dir(fallback_dir());
        session_.layout = layout::Layout(first);
    }
    models_.clear();
    ++epoch_;
    reconcile();
    changed();
    return ok();
}

Result<void> Workspace::delete_layout(std::string_view name) const {
    if (options_.config_root.empty()) {
        return fail(ErrorCode::Unsupported, "no config directory");
    }
    return layout::delete_named_layout(layout::layouts_dir(options_.config_root), name);
}

} // namespace navigator::ui
