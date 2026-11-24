// src/ui/workspace/workspace_view.cpp — panes, tab strips, splitters and tab drag feedback.
// Popups, menus and the key-hint footer are in popups.cpp.
#include "workspace/workspace_view.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <navigator/i18n.hpp>

#include "widgets/icons.hpp"

namespace navigator::ui {
namespace {

// Tab anatomy (dp).
constexpr float kTabPadLeft = 10.0F;
constexpr float kTabIcon = 14.0F;
constexpr float kTabIconGap = 7.0F;
constexpr float kTabClose = 18.0F;
constexpr float kTabCloseGap = 4.0F;
constexpr float kTabPadRight = 5.0F;
constexpr float kTabRadius = 7.0F;
constexpr float kStripTop = 5.0F;    // strip top -> tab top
constexpr float kStripBottom = 3.0F; // tab bottom -> strip bottom
constexpr float kNoIconWidth = 76.0F;

Color faded(const Color& c, float alpha) { return c.with_alpha(c.a * alpha); }

// sRGB "over" of a translucent overlay on an opaque base.
Color over(const Color& base, const Color& overlay) {
    const float a = overlay.a;
    return Color{(base.r * (1.0F - a)) + (overlay.r * a), (base.g * (1.0F - a)) + (overlay.g * a),
                 (base.b * (1.0F - a)) + (overlay.b * a), 1.0F};
}

Palette pane_palette(const Palette& base, Theme theme, float dim) {
    if (dim <= 0.001F) {
        return base;
    }
    return mix(base, dimmed(base, theme), dim);
}

std::string_view edge_label_key(const DropTarget& t) {
    if (t.kind == DropKind::Centre) {
        return "drop.move_here";
    }
    switch (t.edge) {
    case layout::SplitEdge::Left: return "drop.split_left";
    case layout::SplitEdge::Right: return "drop.split_right";
    case layout::SplitEdge::Top: return "drop.split_up";
    case layout::SplitEdge::Bottom: return "drop.split_down";
    }
    return "drop.move_here";
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Views and caches
// ---------------------------------------------------------------------------------------------

BrowserView& WorkspaceView::view(layout::TabId tab) {
    std::unique_ptr<BrowserView>& v = views_[tab.value];
    if (!v) {
        v = std::make_unique<BrowserView>();
    }
    return *v;
}

const BrowserView* WorkspaceView::find_view(layout::TabId tab) const {
    const auto it = views_.find(tab.value);
    return it == views_.end() ? nullptr : it->second.get();
}

void WorkspaceView::prune(const Workspace& ws) {
    if (epoch_ != ws.layout_epoch()) {
        epoch_ = ws.layout_epoch();
        views_.clear();
        strip_state_.clear();
        drag_.cancel();
        splitter_.reset();
        debug_drag_ = false;
        active_ = WidgetId::None;
        return;
    }
    for (auto it = views_.begin(); it != views_.end();) {
        if (ws.layout().find_tab(layout::TabId{it->first})) {
            ++it;
        } else {
            it = views_.erase(it);
        }
    }
    for (auto it = strip_state_.begin(); it != strip_state_.end();) {
        if (ws.layout().find_pane(layout::PaneId{it->first}) != nullptr) {
            ++it;
        } else {
            it = strip_state_.erase(it);
        }
    }
    if (drag_.state() != TabDrag::State::Idle && !ws.layout().find_tab(drag_.tab())) {
        drag_.cancel();
        debug_drag_ = false;
    }
}

void WorkspaceView::clear_caches() noexcept {
    fitter_.clear();
    for (auto& [id, v] : views_) {
        v->clear_caches();
    }
}

bool WorkspaceView::animating() const noexcept {
    if (fading_ || drag_.state() != TabDrag::State::Idle || splitter_.has_value() ||
        toast_.has_value() || paths_busy()) {
        return true;
    }
    return std::any_of(views_.begin(), views_.end(),
                       [](const auto& kv) { return kv.second->animating(); });
}

bool WorkspaceView::dragging() const noexcept { return drag_.dragging(); }

void WorkspaceView::cancel_drag() noexcept {
    drag_.cancel();
    debug_drag_ = false;
    if (active_ == WidgetId::Tab) {
        active_ = WidgetId::None;
    }
}

void WorkspaceView::debug_drag(layout::TabId tab, layout::PaneId from, DropTarget target) {
    drag_.press(tab, from, 0.0F, 0.0F);
    (void)drag_.move(1000.0F, 1000.0F, 0.0F);
    drag_.hover(target);
    debug_drag_ = true;
    grab_title_.clear();
    grab_w_ = 0.0F;
    grab_dx_ = 0.0F;
}

std::optional<RectF> WorkspaceView::strip_rect(layout::PaneId pane) const {
    if (const Strip* s = strip_of(pane)) {
        return s->rect;
    }
    return std::nullopt;
}

const WorkspaceView::Strip* WorkspaceView::strip_of(layout::PaneId pane) const {
    for (const Strip& s : strips_) {
        if (s.pane == pane) {
            return &s;
        }
    }
    return nullptr;
}

void WorkspaceView::show_toast(std::string text, bool error, double now_ms) {
    toast_ = Toast{std::move(text), error, now_ms + (error ? 5000.0 : 2600.0)};
}

PointerInput WorkspaceView::view_pointer(const ShellFrame& f) const {
    const PointerInput& ptr = f.pointer;
    if (!shell_capture_ && (!over_splitter_ || ptr.down)) {
        return ptr;
    }
    PointerInput masked;
    masked.x = -1.0e6F;
    masked.y = -1.0e6F;
    masked.inside = false;
    masked.released = ptr.released; // let a captured view finish its gesture
    masked.mods = ptr.mods;
    return masked;
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

void WorkspaceView::layout_strips(const ShellFrame& f) {
    const Painter& p = f.painter;
    const Workspace& ws = f.workspace;
    const layout::Layout& lay = ws.layout();
    const DensityMetrics dm = density_metrics(f.density);
    const RectF area = f.content.w > 0.0F ? f.content : RectF{0.0F, 0.0F, f.width, f.height};
    geom_ = compute_pane_geometry(lay, area, 0.0F, std::round(p.px(kSplitterHit)));
    strips_.clear();
    const float strip_h = std::round(p.px(dm.tab_bar_height));
    const float margin = std::round(p.px(dm.panel_margin));
    const float button = std::round(p.px(kStripButton));
    const float gap = std::round(p.px(kTabGap));
    const bool multi = lay.pane_count() > 1;
    fading_ = false;
    for (const PaneRect& pr : geom_.panes) {
        const layout::PaneNode* node = lay.find_pane(pr.pane);
        if (node == nullptr) {
            continue;
        }
        Strip s;
        s.pane = pr.pane;
        s.pane_rect = pr.rect;
        s.rect = RectF{pr.rect.x, pr.rect.y, pr.rect.w, std::min(strip_h, pr.rect.h)};
        s.body = RectF{pr.rect.x, s.rect.bottom(), pr.rect.w,
                       std::max(pr.rect.bottom() - s.rect.bottom(), 0.0F)};
        s.active_pane = pr.pane == lay.active_pane();

        StripState& st = strip_state_[pr.pane.value];
        const float target = (multi && !s.active_pane) ? 1.0F : 0.0F;
        if (!f.animations) {
            st.dim = target;
        } else if (st.dim != target) {
            const auto step = static_cast<float>(f.dt_ms / motion::pane_dim_ms);
            st.dim =
                target > st.dim ? std::min(target, st.dim + step) : std::max(target, st.dim - step);
            fading_ = fading_ || st.dim != target;
        }
        s.dim = st.dim;

        const float tab_h = std::round(s.rect.h - p.px(kStripTop + kStripBottom));
        const float tab_y = s.rect.y + std::round(p.px(kStripTop));
        float right = s.rect.right() - margin;
        // The window's theme switch lives at the right end of the top-right pane's strip.
        if (!f.sidebar_visible && pr.rect.y <= 0.5F && pr.rect.right() >= area.right() - 0.5F) {
            s.theme =
                RectF{right - button, tab_y + std::round(0.5F * (tab_h - button)), button, button};
            right = s.theme.x - std::round(p.px(6.0F));
        }
        const float left = s.rect.x + margin;
        const float plus_w = button + std::round(p.px(4.0F));

        // Natural widths; colliding names get their distinguishing parent folder.
        std::vector<Path> dirs;
        dirs.reserve(node->tabs.size());
        for (const layout::Tab& t : node->tabs) {
            const BrowserModel* m = ws.model(t.id);
            dirs.push_back(m != nullptr && m->loaded() ? m->current_dir() : t.state.path);
        }
        std::vector<std::string> titles = disambiguated_titles(dirs);
        if (f.tab_title) {
            for (std::size_t i = 0; i < node->tabs.size() && i < titles.size(); ++i) {
                if (std::optional<std::string> t = f.tab_title(node->tabs[i].id); t) {
                    titles[i] = std::move(*t);
                }
            }
        }
        std::vector<float> natural;
        natural.reserve(node->tabs.size());
        for (std::size_t i = 0; i < node->tabs.size(); ++i) {
            const layout::Tab& t = node->tabs[i];
            TabSlot slot;
            slot.id = t.id;
            slot.active = i == node->active_tab;
            slot.title = titles[i];
            const ShapedText label =
                p.shape(slot.active ? type::tab_active : type::tab, slot.title);
            natural.push_back(p.px(kTabPadLeft + kTabIcon + kTabIconGap + kTabCloseGap + kTabClose +
                                   kTabPadRight) +
                              label.width());
            s.slots.push_back(std::move(slot));
        }
        TabStripInput in;
        in.natural = natural;
        in.min_width = std::round(p.px(kTabMinWidth));
        in.max_width = std::round(p.px(kTabMaxWidth));
        in.gap = gap;
        in.available = std::max(right - left - plus_w, 0.0F);
        in.scroll = st.scroll;
        in.active = node->active_tab;
        s.layout = layout_tab_strip(in);
        if (s.layout.overflow) {
            in.available = std::max(right - left - plus_w - button - std::round(p.px(4.0F)), 0.0F);
            s.layout = layout_tab_strip(in);
            s.more =
                RectF{right - button, tab_y + std::round(0.5F * (tab_h - button)), button, button};
        }
        // Keep the active tab in view when it changes (or the strip starts to overflow).
        const layout::TabId active_id = node->active_tab < node->tabs.size()
                                            ? node->tabs[node->active_tab].id
                                            : layout::TabId{};
        if (st.shown_active != active_id) {
            st.shown_active = active_id;
            s.layout.scroll =
                scroll_to_show(s.layout, node->active_tab, in.available, s.layout.scroll);
        }
        // Wheel over the strip scrolls it.
        const PointerInput& ptr = f.pointer;
        if (s.layout.overflow && contains(s.rect, ptr.x, ptr.y) && !popup_open() &&
            (ptr.wheel_y != 0.0F || ptr.wheel_x != 0.0F)) {
            s.layout.scroll =
                std::clamp(s.layout.scroll - ((ptr.wheel_y - ptr.wheel_x) * p.px(48.0F)), 0.0F,
                           s.layout.max_scroll(in.available));
        }
        st.scroll = s.layout.scroll;
        s.tabs = RectF{left, tab_y, in.available, tab_h};
        for (std::size_t i = 0; i < s.slots.size(); ++i) {
            s.slots[i].rect = RectF{std::round(left + s.layout.x[i] - s.layout.scroll), tab_y,
                                    s.layout.w[i], tab_h};
        }
        const float tabs_end =
            s.slots.empty() ? left : std::min(s.slots.back().rect.right(), s.tabs.right());
        s.plus = RectF{tabs_end + std::round(p.px(4.0F)),
                       tab_y + std::round(0.5F * (tab_h - button)), button, button};
        strips_.push_back(std::move(s));
    }
}

std::vector<PaneDropArea> WorkspaceView::drop_areas() const {
    std::vector<PaneDropArea> out;
    for (const Strip& s : strips_) {
        PaneDropArea a;
        a.pane = s.pane;
        a.rect = s.pane_rect;
        a.strip = s.rect;
        for (const TabSlot& t : s.slots) {
            a.tab_center.push_back(t.rect.center_x());
        }
        out.push_back(std::move(a));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

BrowserFrame WorkspaceView::browser_frame(const ShellFrame& f, const Strip& s, layout::TabId tab,
                                          const BrowserModel& m, const Palette& pal,
                                          const PointerInput& ptr) const {
    const bool filter_focused = f.filter_focus == tab && s.active_pane;
    std::optional<search::IndexStatus> index;
    if (f.index_status && m.flatten_is_volume()) {
        index = f.index_status(m.flatten_root());
    }
    return BrowserFrame{
        .painter = f.painter,
        .palette = pal,
        .theme = f.theme,
        .model = m,
        .keymap = f.keymap,
        .pointer = ptr,
        .queue = f.queue,
        .intents = f.intents,
        .width = f.width,
        .height = f.height,
        .bounds = s.body,
        .tab = tab.value,
        // While the filter field has the keyboard the list cursor draws as inactive (B1).
        .focused = f.focused && s.active_pane && f.list_keyboard && !filter_focused,
        .pane_active = s.active_pane,
        .marked = s.active_pane && f.workspace.layout().pane_count() > 1,
        .animations = f.animations,
        .alternating_rows = f.alternating_rows,
        .density = f.density,
        .spacing_scale_percent = f.spacing_scale_percent,
        .dt_ms = f.dt_ms,
        .now = f.now,
        .home = f.home,
        .address = address_.is_open() && address_.tab == tab.value ? &address_.edit() : nullptr,
        .address_error = !address_.error().empty(),
        .bookmarked =
            f.bookmarks != nullptr && m.loaded() && f.bookmarks->contains(m.current_dir()),
        .filter_focused = filter_focused && f.focused,
        .index_status = std::move(index),
        .thumbnails = f.thumbnails,
        .file_ops = f.file_ops,
        .inline_edit = (f.file_ops != nullptr && f.file_ops->inline_edit() != nullptr &&
                        f.file_ops->inline_edit()->tab == tab && m.loaded() &&
                        f.file_ops->inline_edit()->dir == m.current_dir())
                           ? f.file_ops->inline_edit()
                           : nullptr,
        .slots = f.slots,
        .drag_active = f.file_drag_active,
        .drag_enabled = true,
        .status_reserve =
            s.active_pane && f.ops_chip_width
                ? (f.ops_chip_width() > 0.0F ? f.ops_chip_width() + f.painter.px(10.0F) : 0.0F)
                : 0.0F,
        .git_status = f.git_status,
        .plugins = f.plugins,
    };
}

void WorkspaceView::update(const ShellFrame& f) {
    prune(f.workspace);
    last_now_ms_ = f.now_ms;
    // The address bar edits the active tab only; switching tabs or panes ends the edit.
    if (address_.is_open() && (address_.tab != f.workspace.active_tab().value ||
                               f.workspace.model(f.workspace.active_tab()) == nullptr)) {
        close_address_bar();
    }
    layout_strips(f);
    const PointerInput& ptr = f.pointer;
    shell_capture_ = popup_open() || drag_.state() != TabDrag::State::Idle || splitter_ ||
                     active_ != WidgetId::None;
    bool over_strip = false;
    for (const Strip& s : strips_) {
        over_strip = over_strip || contains(s.rect, ptr.x, ptr.y);
    }
    over_splitter_ =
        !popup_open() && !over_strip && ptr.inside && geom_.splitter_at(ptr.x, ptr.y) != nullptr;
    if (!shell_capture_ && (ptr.pressed || ptr.middle_pressed || ptr.right_pressed) &&
        (over_strip || over_splitter_)) {
        shell_capture_ = true;
    }
    // Phase 7: the operations panel, notices and sheets own the pointer over them.
    if (f.ops_modal || (f.ops_hit && f.ops_hit(ptr.x, ptr.y))) {
        shell_capture_ = true;
    }
    // The address bar's dropdown floats over the list: the list must not see that pointer.
    if (address_.is_open() && address_dropdown_.w > 0.0F &&
        contains(address_dropdown_, ptr.x, ptr.y)) {
        shell_capture_ = true;
    }
    const PointerInput vp = view_pointer(f);
    for (const Strip& s : strips_) {
        const layout::PaneNode* node = f.workspace.layout().find_pane(s.pane);
        if (node == nullptr || node->active_tab >= node->tabs.size()) {
            continue;
        }
        const layout::TabId tab = node->tabs[node->active_tab].id;
        const BrowserModel* m = f.workspace.model(tab);
        if (m == nullptr || (f.is_trash_tab && f.is_trash_tab(tab))) {
            continue;
        }
        const Palette pal = pane_palette(f.palette, f.theme, s.dim);
        const BrowserFrame bf = browser_frame(f, s, tab, *m, pal, vp);
        view(tab).update(bf);
    }
}

void WorkspaceView::draw(const ShellFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const PointerInput& ptr = f.pointer;
    const Workspace& ws = f.workspace;
    r.clear(f.palette.background);
    if (f.draw_sidebar) {
        f.draw_sidebar();
    }

    // The address bar loses focus (and its edit) on a press outside the field and dropdown.
    if (address_.is_open() && !popup_open() &&
        (ptr.pressed || ptr.middle_pressed || ptr.right_pressed)) {
        const BrowserView* bv = find_view(layout::TabId{address_.tab});
        const bool in_field = bv != nullptr && contains(bv->address_rect(), ptr.x, ptr.y);
        if (!in_field && !contains(address_dropdown_, ptr.x, ptr.y)) {
            close_address_bar();
        }
    }
    // A press anywhere in an inactive pane activates it first (docs/keymap.md).
    if (!popup_open() && (ptr.pressed || ptr.middle_pressed || ptr.right_pressed)) {
        const PaneRect* pr = geom_.pane_at(ptr.x, ptr.y);
        if (pr != nullptr && pr->pane != ws.active_pane() &&
            (!over_splitter_ || geom_.splitter_at(ptr.x, ptr.y) == nullptr)) {
            Intent in;
            in.kind = Intent::Kind::ActivatePane;
            in.pane = pr->pane;
            f.intents.push_back(in);
        }
    }
    handle_splitters(f);

    const PointerInput vp = view_pointer(f);
    const DensityMetrics dm = density_metrics(f.density);
    const float margin = std::round(p.px(dm.panel_margin));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    for (Strip& s : strips_) {
        const layout::PaneNode* node = ws.layout().find_pane(s.pane);
        if (node == nullptr) {
            continue;
        }
        const Palette pal = pane_palette(f.palette, f.theme, s.dim);
        if (node->active_tab < node->tabs.size()) {
            const layout::TabId tab = node->tabs[node->active_tab].id;
            if (f.is_trash_tab && f.draw_trash && f.is_trash_tab(tab)) {
                r.set_clip(s.body);
                f.draw_trash(tab, s.body, pal, vp, s.active_pane,
                             s.active_pane && ws.layout().pane_count() > 1);
                r.set_clip(std::nullopt);
            } else if (const BrowserModel* m = ws.model(tab); m != nullptr) {
                const BrowserFrame bf = browser_frame(f, s, tab, *m, pal, vp);
                r.set_clip(s.body);
                view(tab).draw(bf);
                r.set_clip(std::nullopt);
            }
            // The progress chip lives in the active pane's status bar.
            if (s.active_pane && f.ops_chip_width && f.draw_ops_chip) {
                const float cw = f.ops_chip_width();
                if (cw > 0.0F) {
                    const float sh = std::round(p.px(dm.status_bar_height));
                    const float right_pad = std::round(p.px(dm.panel_margin + 8.0F));
                    r.set_clip(s.body);
                    f.draw_ops_chip(
                        RectF{s.body.right() - right_pad - cw, s.body.bottom() - sh, cw, sh});
                    r.set_clip(std::nullopt);
                }
            }
            // Tabs accept dropped files: that tab's folder.
            if (f.slots != nullptr) {
                for (const TabSlot& t : s.slots) {
                    const BrowserModel* tm = ws.model(t.id);
                    if (tm == nullptr || !tm->loaded() || tm->current_dir().empty() ||
                        (f.is_trash_tab && f.is_trash_tab(t.id))) {
                        continue;
                    }
                    f.slots->push_back(DropSlot{intersect(t.rect, s.tabs), tm->current_dir(),
                                                SlotKind::Tab, t.id.value, t.title});
                }
            }
        }
        // A pane below another one: a hairline separates the upper status line from this strip.
        if (s.pane_rect.y > 0.5F) {
            r.fill_rect(
                RectF{s.pane_rect.x + margin, s.pane_rect.y, s.pane_rect.w - (2.0F * margin), hair},
                f.palette.border);
        }
        draw_strip(f, s, pal);
    }

    // Splitter feedback: an accent line on hover and while dragging.
    for (const SplitterRect& sp : geom_.splitters) {
        const bool dragging =
            splitter_ && splitter_->split == sp.split && splitter_->index == sp.index;
        const bool hover = !splitter_ && over_splitter_ && contains(sp.hit, ptr.x, ptr.y) &&
                           drag_.state() == TabDrag::State::Idle && active_ == WidgetId::None;
        if (!dragging && !hover) {
            continue;
        }
        const float t = std::max(2.0F, std::round(p.px(2.0F)));
        const Color c = faded(f.palette.accent, dragging ? 0.9F : 0.55F);
        const float inset = std::round(p.px(10.0F));
        if (sp.axis == layout::SplitAxis::Horizontal) {
            const float x = std::round(sp.slot.center_x() - (0.5F * t));
            r.fill_rounded_rect(RectF{x, sp.slot.y + inset, t, sp.slot.h - (2.0F * inset)},
                                0.5F * t, c);
        } else {
            const float y = std::round(sp.slot.center_y() - (0.5F * t));
            r.fill_rounded_rect(RectF{sp.slot.x + inset, y, sp.slot.w - (2.0F * inset), t},
                                0.5F * t, c);
        }
    }

    if (f.draw_inspector) {
        f.draw_inspector();
    }
    if (f.draw_sidebar_overlay) {
        f.draw_sidebar_overlay();
    }
    handle_drag(f);
    draw_drag_overlay(f);
    draw_address_dropdown(f);
    draw_toast(f);
    if (f.draw_ops_overlay) {
        f.draw_ops_overlay(toast_ ? std::round(p.px(46.0F)) : 0.0F);
    }
    if (menu_request_) {
        menu_request_ = false;
        if (!popup_open() && !drag_.dragging()) {
            if (const Strip* s = strip_of(ws.active_pane()); s != nullptr) {
                for (const TabSlot& t : s->slots) {
                    if (t.active) {
                        const RectF v = intersect(t.rect, s->tabs);
                        open_tab_menu(f, t.id, std::max(v.x, s->tabs.x),
                                      t.rect.bottom() + p.px(4.0F));
                        popup_.pointer_moved = false; // keyboard: the pointer does not steer
                        popup_.last_px = ptr.x;
                        popup_.last_py = ptr.y;
                    }
                }
            }
        }
    }
    if (f.show_fps) {
        draw_fps_overlay(p, f.palette, f.width, f.height, f.density, f.fps, f.frame_ms,
                         f.render_driver);
    }
    draw_popup(f);

    // Cursor feedback.
    cursor_ = PointerCursor::Default;
    const SplitterRect* hot = nullptr;
    if (splitter_) {
        for (const SplitterRect& sp : geom_.splitters) {
            if (sp.split == splitter_->split && sp.index == splitter_->index) {
                hot = &sp;
            }
        }
    } else if (over_splitter_ && drag_.state() == TabDrag::State::Idle && !popup_open()) {
        hot = geom_.splitter_at(ptr.x, ptr.y);
    }
    if (hot != nullptr) {
        cursor_ = hot->axis == layout::SplitAxis::Horizontal ? PointerCursor::ResizeHorizontal
                                                             : PointerCursor::ResizeVertical;
    }
    if (ptr.released) {
        active_ = WidgetId::None;
    }
    if (ptr.middle_released) {
        middle_tab_ = layout::TabId{};
        middle_plus_pane_ = 0;
    }
    if (toast_ && f.now_ms >= toast_->until_ms) {
        toast_.reset();
    }
}

// ---------------------------------------------------------------------------------------------
// Tab strip
// ---------------------------------------------------------------------------------------------

bool WorkspaceView::strip_button(const ShellFrame& f, const RectF& rect, WidgetId id,
                                 std::size_t index, const Palette& pal, bool middle_too,
                                 bool* middle_fired) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const PointerInput& ptr = f.pointer;
    const bool blocked =
        popup_open() || drag_.dragging() || splitter_.has_value() || f.file_drag_active;
    const bool inside = !blocked && contains(rect, ptr.x, ptr.y);
    if (inside && ptr.pressed && active_ == WidgetId::None) {
        active_ = id;
        active_id_ = static_cast<std::uint32_t>(index);
    }
    const bool mine = active_ == id && active_id_ == static_cast<std::uint32_t>(index);
    bool fired = mine && ptr.released && inside;
    if (middle_too && middle_fired != nullptr) {
        if (inside && ptr.middle_pressed) {
            middle_plus_pane_ = static_cast<std::uint32_t>(index);
        }
        *middle_fired =
            inside && ptr.middle_released && middle_plus_pane_ == static_cast<std::uint32_t>(index);
    }
    const bool hover = inside && (active_ == WidgetId::None || mine);
    if (mine && inside && ptr.down) {
        r.fill_rounded_rect(rect, p.px(radius::sm + 1.0F), pal.control_pressed);
    } else if (hover) {
        r.fill_rounded_rect(rect, p.px(radius::sm + 1.0F), pal.control_hover);
    }
    return fired;
}

void WorkspaceView::draw_tab(const ShellFrame& f, const Strip& s, const TabSlot& t,
                             const Palette& pal, bool hover, float alpha, bool floating) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const RectF rect = snap(t.rect);
    const float rad = std::round(p.px(kTabRadius));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    if (floating) {
        r.drop_shadow(rect, rad, p.px(14.0F), p.px(4.0F), pal.shadow);
    }
    if (t.active || floating) {
        // Raised pill. Light theme: surface_raised equals the list surface, so a hairline and a
        // whisper of shadow lift it off the strip.
        if (!floating) {
            r.drop_shadow(rect, rad, p.px(3.0F), p.px(1.0F), faded(pal.shadow, 0.5F * alpha));
        }
        r.fill_rounded_rect(rect, rad, faded(pal.surface_raised, alpha));
        r.stroke_rounded_rect(rect, rad, hair, faded(pal.border, alpha));
        if (s.active_pane && t.active && !floating) {
            // Active pane marker: a short accent edge along the top of its active tab.
            const float bar_h = std::max(2.0F, std::round(p.px(2.0F)));
            r.fill_rounded_rect(RectF{rect.x + rad, rect.y + hair, rect.w - (2.0F * rad), bar_h},
                                0.5F * bar_h, faded(pal.accent, alpha));
        }
    } else if (hover) {
        r.fill_rounded_rect(rect, rad, faded(pal.control_hover, alpha));
    }

    const float mid = rect.y + (0.5F * rect.h);
    float x = rect.x + std::round(p.px(kTabPadLeft));
    const bool show_icon = rect.w >= p.px(kNoIconWidth);
    if (show_icon) {
        const float icon = std::round(p.px(kTabIcon));
        draw_folder_glyph(r, pal, x, std::round(mid - (0.5F * icon)), icon,
                          alpha * (t.active || hover ? 1.0F : 0.85F));
        x += icon + std::round(p.px(kTabIconGap));
    }
    const float close = std::round(p.px(kTabClose));
    const float close_x = rect.right() - std::round(p.px(kTabPadRight)) - close;
    // The close slot is only reserved while the button shows (hover / active tab).
    const bool shows_close = (hover || t.active) && !floating;
    const float label_room = shows_close ? close_x - std::round(p.px(kTabCloseGap)) - x
                                         : rect.right() - std::round(p.px(kTabPadLeft)) - x;
    const ShapedText label =
        fitter_.fit(p, t.active ? type::tab_active : type::tab, t.title, label_room, Elide::Middle);
    const Color ink = (t.active || hover || floating) ? pal.text_primary : pal.text_secondary;
    p.draw(label, x, std::round(mid + (0.5F * label.cap_height())), faded(ink, alpha));

    if (shows_close) {
        const RectF cr = snap(RectF{close_x, mid - (0.5F * close), close, close});
        const PointerInput& ptr = f.pointer;
        const bool over = hover && contains(cr, ptr.x, ptr.y) && !drag_.dragging();
        if (over) {
            r.fill_rounded_rect(cr, p.px(radius::sm), pal.control_pressed);
        }
        draw_cross(r, cr.x + (0.5F * cr.w), mid, p.px(3.6F), std::max(1.0F, p.px(1.3F)),
                   faded(over ? pal.text_primary : pal.text_secondary, alpha));
    }
}

void WorkspaceView::draw_strip(const ShellFrame& f, Strip& s, const Palette& pal) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const PointerInput& ptr = f.pointer;
    const bool blocked = popup_open() || splitter_.has_value();
    const bool dragging = drag_.dragging();
    const float close = std::round(p.px(kTabClose));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));

    // Interaction first (the drawn hover state then matches the press).
    std::size_t hovered = s.slots.size();
    bool over_widget = false;
    for (std::size_t i = 0; i < s.slots.size(); ++i) {
        const TabSlot& t = s.slots[i];
        const RectF visible = intersect(t.rect, s.tabs);
        const bool inside = !blocked && visible.w > 0.0F && contains(visible, ptr.x, ptr.y);
        if (t.id == debug_hover_tab_ || (inside && !dragging &&
                                         (active_ == WidgetId::None || active_ == WidgetId::Tab ||
                                          active_ == WidgetId::TabClose))) {
            hovered = i;
        }
        if (!inside) {
            continue;
        }
        over_widget = true;
        const float mid = t.rect.y + (0.5F * t.rect.h);
        const RectF cr{t.rect.right() - std::round(p.px(kTabPadRight)) - close,
                       mid - (0.5F * close), close, close};
        const bool on_close = contains(cr, ptr.x, ptr.y);
        if (ptr.pressed && active_ == WidgetId::None && !dragging) {
            if (on_close) {
                active_ = WidgetId::TabClose;
                active_id_ = t.id.value;
            } else {
                active_ = WidgetId::Tab;
                active_id_ = t.id.value;
                drag_.press(t.id, s.pane, ptr.x, ptr.y);
                grab_dx_ = ptr.x - t.rect.x;
                grab_w_ = t.rect.w;
                grab_title_ = t.title;
            }
        }
        if (ptr.released && active_ == WidgetId::TabClose && active_id_ == t.id.value && on_close) {
            f.queue.enqueue(CommandId::CloseTab, t.id.value);
        }
        if (ptr.middle_pressed) {
            middle_tab_ = t.id;
        }
        if (ptr.middle_released && middle_tab_ == t.id) {
            f.queue.enqueue(CommandId::CloseTab, t.id.value);
        }
        if (ptr.right_pressed && !dragging) {
            open_tab_menu(f, t.id, ptr.x, ptr.y);
        }
    }

    // Tabs.
    r.set_clip(s.tabs);
    for (std::size_t i = 0; i < s.slots.size(); ++i) {
        const TabSlot& t = s.slots[i];
        const bool lifted = dragging && t.id == drag_.tab();
        // Hairline separators between quiet neighbours.
        if (i + 1 < s.slots.size()) {
            const TabSlot& n = s.slots[i + 1];
            // No separator next to a tab the overflow clips (it would float in the fade).
            const bool clipped =
                t.rect.x < s.tabs.x - 0.5F || n.rect.right() > s.tabs.right() + 0.5F;
            const bool quiet =
                !t.active && !n.active && hovered != i && hovered != i + 1 && !lifted && !clipped;
            if (quiet) {
                const float sx =
                    std::round(t.rect.right() + (0.5F * p.px(kTabGap)) - (0.5F * hair));
                const float sh = std::round(p.px(14.0F));
                r.fill_rect(RectF{sx, std::round(t.rect.center_y() - (0.5F * sh)), hair, sh},
                            pal.border);
            }
        }
        draw_tab(f, s, t, pal, hovered == i, lifted ? 0.35F : 1.0F, false);
    }
    // Overflow: clipped edges fade into the strip instead of ending in a hard cut.
    if (s.layout.overflow) {
        const float fade = std::round(p.px(18.0F));
        constexpr int kSteps = 9;
        const float step = std::max(1.0F, std::ceil(fade / static_cast<float>(kSteps)));
        const bool left = s.layout.scroll > 0.5F;
        const bool right = s.layout.scroll < s.layout.max_scroll(s.tabs.w) - 0.5F;
        for (int k = 0; k < kSteps; ++k) {
            const float a = 1.0F - (static_cast<float>(k) / static_cast<float>(kSteps));
            const Color c = f.palette.background.with_alpha(a);
            const float off = static_cast<float>(k) * step;
            if (left) {
                r.fill_rect(RectF{s.tabs.x + off, s.rect.y, step, s.rect.h}, c);
            }
            if (right) {
                r.fill_rect(RectF{s.tabs.right() - off - step, s.rect.y, step, s.rect.h}, c);
            }
        }
    }
    r.set_clip(std::nullopt);

    // Active pane of a split layout: a full-width accent underline under its strip (design
    // review B1: a pane-level marker that is not a tint).
    if (s.active_pane && f.workspace.layout().pane_count() > 1) {
        const float t = std::max(2.0F, std::round(p.px(2.0F)));
        const float m = std::round(p.px(density_metrics(f.density).panel_margin));
        r.fill_rounded_rect(RectF{s.rect.x + m, s.rect.bottom() - t, s.rect.w - (2.0F * m), t},
                            0.5F * t, pal.accent);
    }

    // "+" (middle-click: new tab on the home folder).
    bool middle = false;
    if (strip_button(f, s.plus, WidgetId::TabPlus, s.pane.value, pal, true, &middle)) {
        f.queue.enqueue(CommandId::NewTab, NewTabArg::None);
    }
    if (middle) {
        f.queue.enqueue(CommandId::NewTab, NewTabArg::Home);
    }
    const bool plus_hot = contains(s.plus, ptr.x, ptr.y) && !blocked && !dragging;
    draw_plus(r, s.plus.center_x(), s.plus.center_y(), p.px(5.0F), std::max(1.0F, p.px(1.5F)),
              plus_hot ? pal.text_primary : pal.text_secondary);
    over_widget = over_widget || contains(s.plus, ptr.x, ptr.y);

    if (s.more.w > 0.0F) {
        if (strip_button(f, s.more, WidgetId::TabMore, s.pane.value, pal, false, nullptr)) {
            open_tab_list(f, s);
        }
        const bool hot = contains(s.more, ptr.x, ptr.y) && !blocked;
        draw_chevron(r, s.more.center_x(), s.more.center_y() + p.px(1.0F), p.px(8.0F),
                     Direction::Down, std::max(1.0F, p.px(1.5F)),
                     hot ? pal.text_primary : pal.text_secondary);
        over_widget = over_widget || contains(s.more, ptr.x, ptr.y);
    }
    if (s.theme.w > 0.0F) {
        if (strip_button(f, s.theme, WidgetId::Theme, s.pane.value, pal, false, nullptr)) {
            f.queue.enqueue(CommandId::ToggleTheme);
        }
        const bool hot = contains(s.theme, ptr.x, ptr.y) && !blocked;
        Color behind = f.palette.background;
        if (hot) {
            behind = over(behind, ptr.down ? pal.control_pressed : pal.control_hover);
        }
        draw_theme_glyph(r, s.theme.center_x(), s.theme.center_y(), p.px(7.0F), f.theme,
                         hot ? pal.text_primary : pal.text_secondary, behind);
        over_widget = over_widget || contains(s.theme, ptr.x, ptr.y);
    }

    // Double-click on empty strip space opens a new tab.
    if (!blocked && !dragging && ptr.pressed && ptr.clicks >= 2 && !over_widget &&
        contains(s.rect, ptr.x, ptr.y) && active_ == WidgetId::None) {
        f.queue.enqueue(CommandId::NewTab, NewTabArg::None);
    }
}

// ---------------------------------------------------------------------------------------------
// Splitters and tab drag
// ---------------------------------------------------------------------------------------------

void WorkspaceView::handle_splitters(const ShellFrame& f) {
    const PointerInput& ptr = f.pointer;
    if (popup_open()) {
        splitter_.reset();
        return;
    }
    if (splitter_) {
        const SplitterRect* s = nullptr;
        for (const SplitterRect& sp : geom_.splitters) {
            if (sp.split == splitter_->split && sp.index == splitter_->index) {
                s = &sp;
            }
        }
        if (s == nullptr || !ptr.down) {
            splitter_.reset();
            return;
        }
        const float coord = s->axis == layout::SplitAxis::Horizontal ? ptr.x : ptr.y;
        if (coord != splitter_->last) {
            splitter_->last = coord;
            const float min_px = f.painter.px(
                s->axis == layout::SplitAxis::Horizontal ? kMinPaneWidth : kMinPaneHeight);
            Intent in;
            in.kind = Intent::Kind::MoveSplitter;
            in.split = s->split;
            in.index = s->index;
            in.position = splitter_position(*s, coord);
            in.min_fraction = min_fraction(*s, min_px);
            f.intents.push_back(in);
        }
        return;
    }
    if (!ptr.pressed || !over_splitter_ || active_ != WidgetId::None ||
        drag_.state() != TabDrag::State::Idle) {
        return;
    }
    const SplitterRect* s = geom_.splitter_at(ptr.x, ptr.y);
    if (s == nullptr) {
        return;
    }
    if (ptr.clicks >= 2) {
        Intent in;
        in.kind = Intent::Kind::EqualizeSplit;
        in.split = s->split;
        f.intents.push_back(in);
        active_ = WidgetId::Splitter;
        return;
    }
    splitter_ = SplitterDrag{s->split, s->index, -1.0F};
    active_ = WidgetId::Splitter;
}

void WorkspaceView::handle_drag(const ShellFrame& f) {
    if (debug_drag_) {
        return;
    }
    const PointerInput& ptr = f.pointer;
    if (drag_.state() == TabDrag::State::Idle) {
        return;
    }
    if (popup_open()) {
        drag_.cancel();
        return;
    }
    if (drag_.state() == TabDrag::State::Pressed && ptr.down) {
        (void)drag_.move(ptr.x, ptr.y, f.painter.px(kDragThreshold));
    }
    if (drag_.dragging()) {
        DropZoneMetrics m;
        m.band_max = f.painter.px(160.0F);
        drag_.hover(hit_test_drop(drop_areas(), ptr.x, ptr.y, m));
    }
    if (ptr.released) {
        const TabDrag::Outcome o = drag_.release();
        Intent in;
        in.tab = o.tab.value;
        if (o.kind == TabDrag::Outcome::Kind::Click) {
            in.kind = Intent::Kind::ActivateTab;
            f.intents.push_back(in);
        } else if (o.kind == TabDrag::Outcome::Kind::Drop) {
            in.kind = Intent::Kind::DropTab;
            in.drop = o.target;
            f.intents.push_back(in);
        }
    } else if (!ptr.down) {
        drag_.cancel(); // the release was lost (focus change)
    }
}

void WorkspaceView::draw_drag_overlay(const ShellFrame& f) {
    if (!drag_.dragging()) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const DropTarget& t = drag_.target();
    const layout::Layout& lay = f.workspace.layout();
    const layout::PaneNode* origin = lay.find_pane(drag_.origin_pane());
    const bool sole_tab = origin != nullptr && origin->tabs.size() <= 1;
    const Strip* target = strip_of(t.pane);

    if (target != nullptr && t.kind == DropKind::TabBar) {
        float x = target->tabs.x;
        if (t.index < target->slots.size()) {
            x = target->slots[t.index].rect.x - (0.5F * p.px(kTabGap));
        } else if (!target->slots.empty()) {
            x = target->slots.back().rect.right() + (0.5F * p.px(kTabGap));
        }
        const float w = std::max(2.0F, std::round(p.px(2.0F)));
        const RectF bar = snap(
            RectF{x - (0.5F * w), target->tabs.y - p.px(1.0F), w, target->tabs.h + p.px(2.0F)});
        r.fill_rounded_rect(bar, 0.5F * w, pal.accent);
        r.fill_circle(bar.center_x(), bar.y, p.px(3.0F), pal.accent);
        r.fill_circle(bar.center_x(), bar.bottom(), p.px(3.0F), pal.accent);
    } else if (target != nullptr && (t.kind == DropKind::Edge || t.kind == DropKind::Centre)) {
        const bool no_op =
            t.pane == drag_.origin_pane() && (t.kind == DropKind::Centre || sole_tab);
        if (!no_op) {
            PaneDropArea area;
            area.pane = target->pane;
            area.rect = target->pane_rect;
            area.strip = target->rect;
            const float inset = std::round(p.px(6.0F));
            RectF pv = drop_preview(area, t);
            pv = snap(
                RectF{pv.x + inset, pv.y + inset, pv.w - (2.0F * inset), pv.h - (2.0F * inset)});
            const float rad = std::round(p.px(radius::lg));
            r.fill_rounded_rect(pv, rad, faded(pal.accent, 0.14F));
            r.stroke_rounded_rect(pv, rad, std::max(1.0F, std::round(p.px(1.5F))),
                                  faded(pal.accent, 0.80F));
            // Label pill in the preview's centre.
            const ShapedText label = p.shape(type::button, tr(edge_label_key(t)));
            const float ph = std::round(p.px(30.0F));
            const float pw = std::round(label.width() + p.px(28.0F));
            const RectF pill =
                snap(RectF{pv.center_x() - (0.5F * pw), pv.center_y() - (0.5F * ph), pw, ph});
            if (pill.w < pv.w && pill.h < pv.h) {
                r.drop_shadow(pill, 0.5F * ph, p.px(10.0F), p.px(2.0F), pal.shadow);
                r.fill_rounded_rect(pill, 0.5F * ph, pal.accent);
                p.draw(label, pill.x + std::round(p.px(14.0F)),
                       std::round(pill.center_y() + (0.5F * label.cap_height())), pal.on_accent);
            }
        }
    }

    // The tab follows the pointer.
    const PointerInput& ptr = f.pointer;
    if (grab_w_ <= 0.0F) {
        for (const Strip& s : strips_) {
            for (const TabSlot& slot : s.slots) {
                if (slot.id == drag_.tab()) {
                    grab_title_ = slot.title;
                    grab_w_ = slot.rect.w;
                    grab_dx_ = 0.5F * slot.rect.w;
                }
            }
        }
    }
    {
        Strip ghost_strip;
        ghost_strip.active_pane = true;
        TabSlot ghost;
        ghost.title = grab_title_;
        ghost.active = true;
        const float h = strips_.empty() ? p.px(26.0F) : strips_.front().tabs.h;
        float gx = ptr.x - grab_dx_;
        float gy = ptr.y - (0.5F * h);
        if (debug_drag_ && target != nullptr) {
            // Screenshots: park the floating tab near the pointer position a user would have.
            const RectF pv =
                drop_preview(PaneDropArea{target->pane, target->pane_rect, target->rect, {}}, t);
            gx = pv.center_x() - (0.25F * grab_w_);
            gy = pv.center_y() + p.px(30.0F);
        }
        // Keep the floating tab inside the window.
        const float edge = p.px(4.0F);
        gx = std::clamp(gx, edge, std::max(edge, f.width - grab_w_ - edge));
        gy = std::clamp(gy, edge, std::max(edge, f.height - h - edge));
        ghost.rect = RectF{std::round(gx), std::round(gy), grab_w_, h};
        draw_tab(f, ghost_strip, ghost, pal, false, 0.96F, true);
    }
}

void WorkspaceView::draw_toast(const ShellFrame& f) {
    if (!toast_) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const ShapedText text = p.shape(type::notice_title, toast_->text);
    const float h = std::round(p.px(36.0F));
    const float icon = toast_->error ? p.px(22.0F) : 0.0F;
    const float w = std::min(std::round(text.width() + icon + p.px(32.0F)), f.width - p.px(32.0F));
    const float bottom = f.height - p.px(density_metrics(f.density).status_bar_height + 14.0F);
    const RectF box = snap(RectF{(0.5F * f.width) - (0.5F * w), bottom - h, w, h});
    const float rad = 0.5F * h;
    r.drop_shadow(box, rad, p.px(18.0F), p.px(4.0F), pal.shadow);
    r.fill_rounded_rect(box, rad, toast_->error ? pal.danger_surface : pal.surface_raised);
    r.stroke_rounded_rect(box, rad, std::max(1.0F, std::round(p.px(1.0F))),
                          toast_->error ? pal.danger_border : pal.border);
    float x = box.x + std::round(p.px(16.0F));
    if (toast_->error) {
        draw_alert_glyph(r, x + p.px(7.0F), box.center_y(), p.px(7.0F), pal.danger,
                         pal.danger_surface, p.scale());
        x += icon;
    }
    const ShapedText fitted =
        fitter_.fit(p, type::notice_title, toast_->text, box.right() - p.px(16.0F) - x, Elide::End);
    p.draw(fitted, x, std::round(box.center_y() + (0.5F * fitted.cap_height())), pal.text_primary);
}

} // namespace navigator::ui
