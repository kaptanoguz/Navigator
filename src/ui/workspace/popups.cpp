// src/ui/workspace/popups.cpp — WorkspaceView popups: Search Tabs, the layout name prompt, the
// layout picker (load / delete), the tab context menu and the strip's overflow tab list.
//
// Keyboard routing (docs/keymap.md, "Popups"): while a popup is open the App passes every
// command to route_command(); list.cursor_* move the highlight, list.activate confirms,
// list.clear_selection (Escape) steps back (a pending confirmation first, then the popup).
// Text arrives through text_input()/backspace() (SDL text input), never as chords.
#include <algorithm>
#include <cmath>

#include <navigator/fs.hpp>
#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "input/sdl_input.hpp"
#include "widgets/icons.hpp"
#include "widgets/text_field.hpp"
#include "workspace/workspace_view.hpp"

namespace navigator::ui {
namespace {

constexpr float kCardWidth = 620.0F;
constexpr float kPromptWidth = 440.0F;
constexpr float kPickerWidth = 480.0F;
constexpr float kFieldHeight = 46.0F;
constexpr float kRowHeight = 34.0F;
constexpr float kSectionHeight = 26.0F;
constexpr float kFooterHeight = 36.0F;
constexpr std::size_t kMaxRows = 9;
constexpr float kMenuRow = 30.0F;
constexpr std::size_t kMaxNameBytes = 64;

Color faded(const Color& c, float a) { return c.with_alpha(c.a * a); }

std::string chord_text(const Keymap& keymap, std::string_view command, std::uint64_t arg,
                       bool any_arg) {
    const std::vector<KeyChord> chords =
        any_arg ? keymap.chords_for(command) : keymap.chords_for(command, arg);
    if (chords.empty()) {
        return {};
    }
    std::string out;
    for (const std::string& part : chord_labels(chords.front())) {
        if (!out.empty()) {
            out += '+';
        }
        out += part;
    }
    return out;
}

bool contains_folded(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return fs::fold_name(haystack).find(fs::fold_name(needle)) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

bool WorkspaceView::wants_text_input() const noexcept {
    return popup_.kind == PopupKind::SearchTabs || popup_.kind == PopupKind::SaveLayout ||
           popup_.kind == PopupKind::PickLayout || popup_.kind == PopupKind::GoTo ||
           (!popup_open() && address_.is_open());
}

void WorkspaceView::close_popup() noexcept {
    if (popup_.kind == PopupKind::GoTo) {
        goto_.close();
    }
    popup_ = Popup{};
}

TextEdit* WorkspaceView::active_field() noexcept {
    switch (popup_.kind) {
    case PopupKind::SearchTabs:
    case PopupKind::SaveLayout:
    case PopupKind::PickLayout: return &popup_.field;
    case PopupKind::GoTo: return &goto_.edit();
    case PopupKind::TabMenu:
    case PopupKind::TabList:
    case PopupKind::Menu:
    case PopupKind::Types: return nullptr;
    case PopupKind::None: break;
    }
    return address_.is_open() ? &address_.edit() : nullptr;
}

void WorkspaceView::field_changed(double now_ms) {
    switch (popup_.kind) {
    case PopupKind::SearchTabs:
    case PopupKind::SaveLayout:
    case PopupKind::PickLayout:
        popup_.confirm.clear();
        popup_.message.clear();
        refilter();
        return;
    case PopupKind::GoTo: goto_.changed(now_ms); return;
    case PopupKind::TabMenu:
    case PopupKind::TabList:
    case PopupKind::Menu:
    case PopupKind::Types: return;
    case PopupKind::None: break;
    }
    if (address_.is_open()) {
        address_.changed(now_ms);
    }
}

bool WorkspaceView::field_tab(double now_ms) {
    if (popup_.kind == PopupKind::GoTo) {
        return goto_.accept_completion(now_ms);
    }
    if (!popup_open() && address_.is_open()) {
        return address_.accept_completion(now_ms);
    }
    return false;
}

void WorkspaceView::attach_paths(PathWorker* worker) noexcept {
    goto_.attach(worker);
    address_.attach(worker);
}

void WorkspaceView::open_goto(const Workspace& ws, const Path& home,
                              std::vector<GotoSource> sources, double now_ms) {
    drag_.cancel();
    close_address_bar();
    popup_ = Popup{};
    popup_.kind = PopupKind::GoTo;
    const BrowserModel* m = ws.model(ws.active_tab());
    const Path cwd = m != nullptr ? m->current_dir() : home;
    goto_.open({}, false, cwd, home, std::move(sources), now_ms);
}

void WorkspaceView::open_address_bar(const Workspace& ws, const Path& home, double now_ms) {
    if (popup_open()) {
        return;
    }
    drag_.cancel();
    const BrowserModel* m = ws.model(ws.active_tab());
    if (m == nullptr || m->current_dir().empty()) {
        return;
    }
    address_.open(fs::to_display_name(m->current_dir().string()), true, m->current_dir(), home, {},
                  now_ms);
    address_.tab = ws.active_tab().value;
    address_scroll_ = 0.0F;
}

void WorkspaceView::close_address_bar() noexcept {
    address_.close();
    address_.tab = 0;
    path_pressed_.reset();
}

bool WorkspaceView::route_address(const Command& command) {
    if (!address_.is_open() || popup_open()) {
        return false;
    }
    switch (command.id) {
    case CommandId::CursorUp: address_.move(-1); return true;
    case CommandId::CursorDown: address_.move(1); return true;
    case CommandId::CursorPageUp:
        address_.move(-static_cast<std::ptrdiff_t>(kMaxRows));
        return true;
    case CommandId::CursorPageDown:
        address_.move(static_cast<std::ptrdiff_t>(kMaxRows));
        return true;
    case CommandId::Activate: address_.submit(false); return true;
    case CommandId::OpenInNewTab: address_.submit(true); return true;
    case CommandId::ClearSelection: close_address_bar(); return true;
    case CommandId::FocusAddressBar: address_.edit().select_all(); return true;
    default: return false;
    }
}

bool WorkspaceView::poll_paths(double now_ms, std::vector<PathOutcome>& out) {
    bool changed = false;
    if (goto_.is_open()) {
        goto_.tick(now_ms);
        changed = goto_.poll(now_ms) || changed;
        if (std::optional<PathOutcome> o = goto_.take_outcome(); o) {
            out.push_back(std::move(*o));
            close_popup();
            changed = true;
        }
    }
    if (address_.is_open()) {
        address_.tick(now_ms);
        changed = address_.poll(now_ms) || changed;
        if (std::optional<PathOutcome> o = address_.take_outcome(); o) {
            out.push_back(std::move(*o));
            close_address_bar();
            changed = true;
        }
    }
    return changed;
}

bool WorkspaceView::paths_busy() const noexcept {
    return (goto_.is_open() && goto_.busy()) || (address_.is_open() && address_.busy());
}

layout::TabId WorkspaceView::dragged_tab() const noexcept {
    return drag_.dragging() ? drag_.tab() : layout::TabId{};
}

void WorkspaceView::open_menu(const Keymap& keymap, std::vector<MenuEntry> entries, float x,
                              float y) {
    drag_.cancel();
    close_address_bar();
    popup_ = Popup{};
    popup_.kind = PopupKind::Menu;
    popup_.menu_x = x;
    popup_.menu_y = y;
    for (MenuEntry& e : entries) {
        MenuItem m;
        m.label = std::move(e.label);
        if (!e.hint_command.empty()) {
            m.hint_command = chord_text(keymap, e.hint_command, 0, true);
        }
        if (e.command) {
            m.command = *e.command;
        }
        m.intent = std::move(e.intent);
        m.enabled = e.enabled;
        m.separator_before = e.separator_before;
        popup_.menu.push_back(std::move(m));
    }
    popup_.highlight = 0;
    popup_.pointer_moved = true;
    popup_.last_px = x;
    popup_.last_py = y;
}

void WorkspaceView::open_search_tabs(const Workspace& ws, const Path* home) {
    drag_.cancel();
    popup_ = Popup{};
    popup_.kind = PopupKind::SearchTabs;
    popup_.items = collect_tab_search_items(ws.layout(), ws.closed_tabs(), home);
    refilter();
    // Start on the first tab that is not the current one: Enter switches somewhere useful.
    for (std::size_t i = 0; i < popup_.filtered.size(); ++i) {
        if (!popup_.items[popup_.filtered[i]].active) {
            popup_.highlight = i;
            break;
        }
    }
}

void WorkspaceView::open_save_layout(const Workspace& ws) {
    drag_.cancel();
    popup_ = Popup{};
    popup_.kind = PopupKind::SaveLayout;
    popup_.field = TextEdit(kMaxNameBytes);
    if (Result<std::vector<std::string>> names = ws.list_layouts(); names) {
        popup_.names = std::move(*names);
    }
}

void WorkspaceView::open_layout_picker(const Workspace& ws, bool delete_mode) {
    drag_.cancel();
    popup_ = Popup{};
    popup_.kind = PopupKind::PickLayout;
    popup_.delete_mode = delete_mode;
    if (Result<std::vector<std::string>> names = ws.list_layouts(); names) {
        popup_.names = std::move(*names);
    } else {
        popup_.message = std::string(tr("error." + std::string(to_string(names.error().code))));
        popup_.message_error = true;
    }
    refilter();
}

void WorkspaceView::layout_deleted(std::string_view name) {
    std::erase(popup_.names, std::string(name));
    popup_.confirm.clear();
    refilter();
}

void WorkspaceView::open_tab_menu(const ShellFrame& f, layout::TabId tab, float x, float y) {
    popup_ = Popup{};
    popup_.kind = PopupKind::TabMenu;
    popup_.menu_x = x;
    popup_.menu_y = y;
    const auto item = [&](std::string_view key, CommandId id, std::uint64_t arg,
                          std::string_view name, bool enabled, bool sep) {
        MenuItem m;
        m.label = std::string(tr(key));
        m.command = Command{id, arg};
        m.enabled = enabled;
        m.separator_before = sep;
        m.hint_command = chord_text(f.keymap, name, 0, true);
        popup_.menu.push_back(std::move(m));
    };
    const layout::Layout& lay = f.workspace.layout();
    const std::optional<layout::TabLocation> loc = lay.find_tab(tab);
    const bool can_close = lay.tab_count() > 1;
    const bool has_left = loc && loc->index > 0;
    item("menu.new_tab", CommandId::NewTab, NewTabArg::None, "tab.new", true, false);
    item("menu.duplicate_tab", CommandId::DuplicateTab, tab.value, "tab.duplicate", true, false);
    item("menu.close_tab", CommandId::CloseTab, tab.value, "tab.close", can_close, true);
    item("menu.close_left_tabs", CommandId::CloseLeftTabs, tab.value, "tab.close_left", has_left,
         false);
    item("menu.reopen_closed_tab", CommandId::ReopenClosedTab, 0, "tab.reopen_closed",
         !f.workspace.closed_tabs().empty(), true);
    popup_.highlight = 0;
    popup_.pointer_moved = true;
    popup_.last_px = x;
    popup_.last_py = y;
}

void WorkspaceView::open_tab_list(const ShellFrame& f, const Strip& s) {
    popup_ = Popup{};
    popup_.kind = PopupKind::TabList;
    popup_.menu_x = s.more.right();
    popup_.menu_y = s.more.bottom() + f.painter.px(4.0F);
    for (const TabSlot& t : s.slots) {
        MenuItem m;
        m.label = t.title;
        m.tab = t.id;
        if (t.active) {
            popup_.highlight = popup_.menu.size();
        }
        popup_.menu.push_back(std::move(m));
    }
    popup_.pointer_moved = false;
}

void WorkspaceView::refilter() {
    popup_.highlight = 0;
    popup_.scroll = 0.0F;
    if (popup_.kind == PopupKind::SearchTabs) {
        popup_.filtered = filter_tab_search(popup_.items, popup_.query());
    } else if (popup_.kind == PopupKind::PickLayout) {
        popup_.names_filtered.clear();
        for (std::size_t i = 0; i < popup_.names.size(); ++i) {
            if (contains_folded(popup_.names[i], popup_.query())) {
                popup_.names_filtered.push_back(i);
            }
        }
    }
}

std::size_t WorkspaceView::popup_rows() const noexcept {
    switch (popup_.kind) {
    case PopupKind::SearchTabs: return popup_.filtered.size();
    case PopupKind::PickLayout: return popup_.names_filtered.size();
    case PopupKind::TabMenu:
    case PopupKind::TabList:
    case PopupKind::Menu: return popup_.menu.size();
    case PopupKind::Types: return popup_.type_rows.size() + 1; // + "All types"
    case PopupKind::GoTo: return goto_.items().size();
    case PopupKind::SaveLayout:
    case PopupKind::None: break;
    }
    return 0;
}

void WorkspaceView::move_highlight(std::ptrdiff_t delta) {
    const std::size_t n = popup_rows();
    if (n == 0) {
        return;
    }
    const bool menu = popup_.kind == PopupKind::TabMenu || popup_.kind == PopupKind::Menu;
    auto h = static_cast<std::ptrdiff_t>(std::min(popup_.highlight, n - 1));
    const auto last = static_cast<std::ptrdiff_t>(n - 1);
    if (menu) {
        // Wraps and skips disabled items.
        for (std::size_t tries = 0; tries < n; ++tries) {
            h = (h + (delta >= 0 ? 1 : -1) + static_cast<std::ptrdiff_t>(n)) %
                static_cast<std::ptrdiff_t>(n);
            if (popup_.menu[static_cast<std::size_t>(h)].enabled) {
                break;
            }
        }
    } else {
        h = std::clamp(h + delta, std::ptrdiff_t{0}, last);
    }
    popup_.highlight = static_cast<std::size_t>(h);
    popup_.confirm.clear();
    popup_.pointer_moved = false;
}

bool WorkspaceView::route_command(const Command& command, CommandQueue& queue,
                                  std::vector<Intent>& out) {
    if (!popup_open()) {
        return false;
    }
    const auto page = static_cast<std::ptrdiff_t>(kMaxRows - 1);
    if (popup_.kind == PopupKind::GoTo) {
        switch (command.id) {
        case CommandId::Quit:
        case CommandId::ToggleTheme:
        case CommandId::ToggleFpsOverlay: return false;
        case CommandId::CursorUp: goto_.move(-1); break;
        case CommandId::CursorDown: goto_.move(1); break;
        case CommandId::CursorPageUp: goto_.move(-page); break;
        case CommandId::CursorPageDown: goto_.move(page); break;
        case CommandId::Activate: goto_.submit(false); break;
        case CommandId::OpenInNewTab: goto_.submit(true); break;
        case CommandId::ClearSelection: close_popup(); break;
        case CommandId::GoTo: goto_.edit().select_all(); break;
        default: break;
        }
        return true;
    }
    switch (command.id) {
    case CommandId::Quit:
    case CommandId::ToggleTheme:
    case CommandId::ToggleFpsOverlay: return false;
    case CommandId::CursorUp: move_highlight(-1); break;
    case CommandId::CursorDown: move_highlight(1); break;
    case CommandId::CursorPageUp: move_highlight(-page); break;
    case CommandId::CursorPageDown: move_highlight(page); break;
    case CommandId::CursorHome: move_highlight(-static_cast<std::ptrdiff_t>(popup_rows())); break;
    case CommandId::CursorEnd: move_highlight(static_cast<std::ptrdiff_t>(popup_rows())); break;
    case CommandId::Activate: confirm_popup(queue, out); break;
    case CommandId::ToggleTypesPopup:
        if (popup_.kind == PopupKind::Types) {
            close_popup(); // Ctrl+Shift+F again closes it
        }
        break;
    case CommandId::ClearSelection:
        if (!popup_.confirm.empty()) {
            popup_.confirm.clear();
            popup_.message.clear();
        } else {
            close_popup();
        }
        break;
    default: break; // everything else is held while a popup is open
    }
    return true;
}

void WorkspaceView::confirm_popup(CommandQueue& queue, std::vector<Intent>& out) {
    switch (popup_.kind) {
    case PopupKind::None: return;
    case PopupKind::SearchTabs: {
        if (popup_.highlight >= popup_.filtered.size()) {
            return;
        }
        const TabSearchItem& item = popup_.items[popup_.filtered[popup_.highlight]];
        Intent in;
        if (item.source == TabSearchItem::Source::Open) {
            in.kind = Intent::Kind::ActivateTab;
            in.tab = item.tab.value;
        } else {
            in.kind = Intent::Kind::ReopenClosed;
            in.index = item.closed_index;
        }
        out.push_back(in);
        close_popup();
        return;
    }
    case PopupKind::SaveLayout: {
        const std::string name = popup_.query();
        if (name.empty()) {
            popup_.message = std::string(tr("layout.name_required"));
            popup_.message_error = true;
            return;
        }
        if (!layout::is_valid_layout_name(name)) {
            popup_.message = std::string(tr("layout.name_invalid"));
            popup_.message_error = true;
            return;
        }
        const bool exists =
            std::find(popup_.names.begin(), popup_.names.end(), name) != popup_.names.end();
        if (exists && popup_.confirm != name) {
            popup_.confirm = name;
            popup_.message = substitute(tr("layout.replace_confirm"), "name", name);
            popup_.message_error = false;
            return;
        }
        Intent in;
        in.kind = Intent::Kind::SaveLayout;
        in.text = name;
        out.push_back(std::move(in));
        close_popup();
        return;
    }
    case PopupKind::PickLayout: {
        if (popup_.highlight >= popup_.names_filtered.size()) {
            return;
        }
        const std::string name = popup_.names[popup_.names_filtered[popup_.highlight]];
        Intent in;
        // A pending delete confirmation (delete mode, the row's x button or Delete) is what
        // Enter confirms, in either mode.
        if (popup_.delete_mode || popup_.confirm == name) {
            if (popup_.confirm != name) {
                popup_.confirm = name;
                return;
            }
            in.kind = Intent::Kind::DeleteLayout;
            in.text = name;
            out.push_back(std::move(in));
            popup_.confirm.clear();
            return;
        }
        in.kind = Intent::Kind::LoadLayout;
        in.text = name;
        out.push_back(std::move(in));
        close_popup();
        return;
    }
    case PopupKind::TabMenu:
    case PopupKind::Menu: {
        if (popup_.highlight < popup_.menu.size() && popup_.menu[popup_.highlight].enabled) {
            const MenuItem& m = popup_.menu[popup_.highlight];
            if (m.intent) {
                out.push_back(*m.intent);
            } else {
                queue.enqueue(m.command);
            }
            close_popup();
        }
        return;
    }
    case PopupKind::GoTo: goto_.submit(false); return;
    case PopupKind::Types: toggle_type_row(popup_.highlight, out); return;
    case PopupKind::TabList: {
        if (popup_.highlight < popup_.menu.size()) {
            Intent in;
            in.kind = Intent::Kind::ActivateTab;
            in.tab = popup_.menu[popup_.highlight].tab.value;
            out.push_back(in);
            close_popup();
        }
        return;
    }
    }
}

bool WorkspaceView::delete_key(std::vector<Intent>& out) {
    if (popup_.kind != PopupKind::PickLayout) {
        return false;
    }
    if (popup_.highlight < popup_.names_filtered.size()) {
        const std::string name = popup_.names[popup_.names_filtered[popup_.highlight]];
        if (popup_.confirm == name) {
            Intent in;
            in.kind = Intent::Kind::DeleteLayout;
            in.text = name;
            out.push_back(std::move(in));
            popup_.confirm.clear();
        } else {
            popup_.confirm = name; // same two-step confirmation as the delete button
        }
    }
    return true;
}

void WorkspaceView::text_input(std::string_view utf8) {
    TextEdit* field = active_field();
    if (field == nullptr) {
        return;
    }
    for (const char c : utf8) {
        if (static_cast<unsigned char>(c) < 0x20U || c == 0x7F) {
            return; // control characters never reach a name or query
        }
    }
    if (field->insert(utf8)) {
        field_changed(last_now_ms_);
    }
}

void WorkspaceView::backspace() {
    TextEdit* field = active_field();
    if (field != nullptr && field->backspace()) {
        field_changed(last_now_ms_);
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------------------------

RectF WorkspaceView::draw_card(const ShellFrame& f, float width, float height, float top) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    // Scrim: the window recedes, the card comes forward.
    r.fill_rect(RectF{0.0F, 0.0F, f.width, f.height},
                f.theme == Theme::Dark ? Color{0.0F, 0.0F, 0.01F, 0.42F} : faded(pal.shadow, 0.9F));
    const float w = std::min(std::round(p.px(width)), f.width - std::round(p.px(32.0F)));
    const RectF card = snap(RectF{std::round((0.5F * f.width) - (0.5F * w)), top, w, height});
    const float rad = std::round(p.px(radius::lg));
    r.drop_shadow(card, rad, p.px(36.0F), p.px(10.0F), pal.shadow);
    r.fill_rounded_rect(card, rad, pal.surface_raised);
    r.stroke_rounded_rect(card, rad, std::max(1.0F, std::round(p.px(1.0F))), pal.border);
    return card;
}

float WorkspaceView::draw_field(const ShellFrame& f, const RectF& box, std::string_view placeholder,
                                bool search_glyph) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    float x = box.x;
    const float mid = box.center_y();
    if (search_glyph) {
        draw_search_glyph(r, x + p.px(8.0F), mid, p.px(6.0F), std::max(1.0F, p.px(1.6F)),
                          pal.text_secondary, pal.surface_raised);
        x += std::round(p.px(26.0F));
    }
    return draw_text_field(p, pal, popup_.field,
                           RectF{x, box.y, box.right() - x - p.px(8.0F), box.h}, placeholder,
                           popup_.field_scroll);
}

float WorkspaceView::draw_keycap(const ShellFrame& f, std::string_view legend, float x, float cy,
                                 bool measure_only) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const ShapedText k = p.shape(type::keycap, legend);
    const float h = std::round(p.px(20.0F));
    const float w = std::round(std::max(h, k.width() + p.px(12.0F)));
    if (measure_only) {
        return w;
    }
    const RectF cap = snap(RectF{x, cy - (0.5F * h), w, h});
    const float rad = p.px(radius::sm);
    const float edge = std::max(1.0F, std::round(p.px(1.0F)));
    r.fill_rounded_rect(RectF{cap.x, cap.y + edge, cap.w, cap.h}, rad, pal.keycap_edge);
    r.fill_rounded_rect(cap, rad, pal.surface);
    r.stroke_rounded_rect(cap, rad, edge, pal.keycap_border);
    p.draw(k, cap.x + std::round((0.5F * cap.w) - (0.5F * k.width())),
           std::round(cap.center_y() + (0.5F * k.cap_height())), pal.keycap_text);
    return w;
}

void WorkspaceView::draw_footer_hints(
    const ShellFrame& f, const RectF& area,
    const std::vector<std::pair<std::string, std::string>>& hints) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    r.fill_rect(RectF{area.x, area.y, area.w, hair}, pal.border);
    float x = area.x + std::round(p.px(14.0F));
    const float cy = area.center_y() + (0.5F * hair);
    for (const auto& [keys, label] : hints) {
        std::size_t pos = 0;
        while (pos <= keys.size()) {
            const std::size_t sp = keys.find(' ', pos);
            const std::string k =
                keys.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            x += draw_keycap(f, k, x, cy, false) + std::round(p.px(3.0F));
            if (sp == std::string::npos) {
                break;
            }
            pos = sp + 1;
        }
        const ShapedText t = p.shape(type::caption, label);
        p.draw(t, x + std::round(p.px(3.0F)), std::round(cy + (0.5F * t.cap_height())),
               pal.text_muted);
        x += t.width() + std::round(p.px(18.0F));
    }
}

bool WorkspaceView::popup_button(const ShellFrame& f, const RectF& rect, std::string_view label,
                                 bool primary, bool danger, std::size_t index) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const bool inside = contains(rect, ptr.x, ptr.y);
    if (inside && ptr.pressed) {
        popup_.pressed = WidgetId::PopupButton;
        popup_.pressed_index = index;
    }
    const bool mine = popup_.pressed == WidgetId::PopupButton && popup_.pressed_index == index;
    const float rad = std::round(p.px(radius::md - 1.0F));
    const ShapedText t = p.shape(type::button, label);
    Color ink = pal.text_primary;
    if (primary) {
        const Color fill = danger ? pal.danger : pal.accent;
        r.fill_rounded_rect(rect, rad, fill);
        if (inside) {
            r.fill_rounded_rect(rect, rad, faded(pal.on_accent, mine && ptr.down ? 0.18F : 0.10F));
        }
        ink = pal.on_accent;
    } else {
        r.stroke_rounded_rect(rect, rad, std::max(1.0F, std::round(p.px(1.0F))), pal.keycap_border);
        if (inside) {
            r.fill_rounded_rect(rect, rad,
                                mine && ptr.down ? pal.control_pressed : pal.control_hover);
        }
    }
    p.draw(t, std::round(rect.center_x() - (0.5F * t.width())),
           std::round(rect.center_y() + (0.5F * t.cap_height())), ink);
    return mine && ptr.released && inside;
}

// ---------------------------------------------------------------------------------------------
// Popups
// ---------------------------------------------------------------------------------------------

void WorkspaceView::draw_popup(const ShellFrame& f) {
    if (!popup_open()) {
        return;
    }
    const PointerInput& ptr = f.pointer;
    if (ptr.x != popup_.last_px || ptr.y != popup_.last_py) {
        if (popup_.last_px >= 0.0F) {
            popup_.pointer_moved = true;
        }
        popup_.last_px = ptr.x;
        popup_.last_py = ptr.y;
    }
    switch (popup_.kind) {
    case PopupKind::SearchTabs: draw_search_tabs(f); break;
    case PopupKind::SaveLayout: draw_save_layout(f); break;
    case PopupKind::PickLayout: draw_layout_picker(f); break;
    case PopupKind::TabMenu:
    case PopupKind::TabList:
    case PopupKind::Menu: draw_menu(f); break;
    case PopupKind::Types: draw_types(f); break;
    case PopupKind::GoTo: draw_goto(f); break;
    case PopupKind::None: break;
    }
    if (ptr.released && popup_open()) {
        popup_.pressed = WidgetId::None;
    }
    popup_.just_opened = false;
}

void WorkspaceView::draw_search_tabs(const ShellFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;

    // Display rows: section headers + items.
    struct Row {
        bool header = false;
        std::size_t filtered = 0; // index into popup_.filtered
        TabSearchItem::Source source = TabSearchItem::Source::Open;
    };
    std::vector<Row> rows;
    std::size_t open_count = 0;
    std::size_t closed_count = 0;
    for (const std::size_t i : popup_.filtered) {
        (popup_.items[i].source == TabSearchItem::Source::Open ? open_count : closed_count)++;
    }
    for (std::size_t k = 0; k < popup_.filtered.size(); ++k) {
        const TabSearchItem::Source src = popup_.items[popup_.filtered[k]].source;
        if (k == 0 || popup_.items[popup_.filtered[k - 1]].source != src) {
            rows.push_back(Row{true, k, src});
        }
        rows.push_back(Row{false, k, src});
    }
    const float row_h = std::round(p.px(kRowHeight));
    const float sec_h = std::round(p.px(kSectionHeight));
    const float field_h = std::round(p.px(kFieldHeight));
    const float foot_h = std::round(p.px(kFooterHeight));
    const float pad = std::round(p.px(6.0F));
    // Pane positions for the mini maps, in a 1000 x 1000 unit square.
    const PaneGeometry minimap = compute_pane_geometry(
        f.workspace.layout(), RectF{0.0F, 0.0F, 1000.0F, 1000.0F}, 0.0F, 0.0F);
    // Visible list height: up to kMaxRows item rows.
    float list_h = 0.0F;
    std::size_t shown_items = 0;
    for (const Row& row : rows) {
        if (!row.header && shown_items >= kMaxRows) {
            break;
        }
        list_h += row.header ? sec_h : row_h;
        shown_items += row.header ? 0 : 1;
    }
    if (rows.empty()) {
        list_h = std::round(p.px(64.0F));
    }
    const float total_h = field_h + list_h + (2.0F * pad) + foot_h;
    const float top = std::round(std::max(p.px(56.0F), f.height * 0.12F));
    const RectF card = draw_card(f, kCardWidth, total_h, top);
    if (ptr.pressed && !contains(card, ptr.x, ptr.y) && !popup_.just_opened) {
        close_popup();
        return;
    }
    const float inner_x = card.x + std::round(p.px(14.0F));
    draw_field(f, RectF{inner_x, card.y, card.w - std::round(p.px(28.0F)), field_h},
               tr("search_tabs.placeholder"), true);
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    r.fill_rect(RectF{card.x, card.y + field_h, card.w, hair}, pal.border);

    const RectF list{card.x + pad, card.y + field_h + pad, card.w - (2.0F * pad), list_h};
    if (rows.empty()) {
        const ShapedText t = p.shape(type::row, tr("search_tabs.no_match"));
        p.draw(t, std::round(list.center_x() - (0.5F * t.width())),
               std::round(list.center_y() + (0.5F * t.cap_height())), pal.text_muted);
    } else {
        // Scroll (in display rows) so the highlighted item is visible.
        std::size_t hi_row = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!rows[i].header && rows[i].filtered == popup_.highlight) {
                hi_row = i;
            }
        }
        auto first = static_cast<std::size_t>(popup_.scroll);
        const auto fits = [&](std::size_t from, std::size_t to) {
            float h = 0.0F;
            for (std::size_t i = from; i <= to && i < rows.size(); ++i) {
                h += rows[i].header ? sec_h : row_h;
            }
            return h <= list_h + 0.5F;
        };
        if (hi_row < first) {
            first = hi_row > 0 && rows[hi_row - 1].header ? hi_row - 1 : hi_row;
        }
        while (first < hi_row && !fits(first, hi_row)) {
            ++first;
        }
        if (ptr.wheel_y != 0.0F && contains(list, ptr.x, ptr.y)) {
            const auto delta = static_cast<std::ptrdiff_t>(-ptr.wheel_y * 3.0F);
            first = static_cast<std::size_t>(
                std::clamp(static_cast<std::ptrdiff_t>(first) + delta, std::ptrdiff_t{0},
                           static_cast<std::ptrdiff_t>(rows.size()) - 1));
        }
        popup_.scroll = static_cast<float>(first);
        r.set_clip(list);
        float y = list.y;
        for (std::size_t i = first; i < rows.size() && y < list.bottom(); ++i) {
            const Row& row = rows[i];
            if (row.header) {
                const bool open = row.source == TabSearchItem::Source::Open;
                const std::string label =
                    std::string(tr(open ? "search_tabs.open" : "search_tabs.closed")) + "  " +
                    format_count(open ? open_count : closed_count);
                const ShapedText t = p.shape(type::section, label);
                p.draw(t, list.x + std::round(p.px(10.0F)), std::round(y + sec_h - p.px(8.0F)),
                       pal.text_muted);
                y += sec_h;
                continue;
            }
            const TabSearchItem& item = popup_.items[popup_.filtered[row.filtered]];
            const RectF rr = snap(RectF{list.x, y, list.w, row_h});
            const bool inside = contains(rr, ptr.x, ptr.y) && contains(list, ptr.x, ptr.y);
            if (inside && popup_.pointer_moved) {
                popup_.highlight = row.filtered;
            }
            if (inside && ptr.pressed) {
                popup_.pressed = WidgetId::PopupRow;
                popup_.pressed_index = row.filtered;
            }
            const bool hi = popup_.highlight == row.filtered;
            const float rad = std::round(p.px(radius::md - 2.0F));
            if (hi) {
                r.fill_rounded_rect(rr, rad, pal.selection);
                const float mw = std::max(2.0F, std::round(p.px(3.0F)));
                r.fill_rounded_rect(
                    snap(RectF{rr.x + p.px(3.0F), rr.y + p.px(8.0F), mw, rr.h - p.px(16.0F)}),
                    0.5F * mw, pal.selection_marker);
            }
            const float mid = rr.center_y();
            const float icon = std::round(p.px(16.0F));
            float x = rr.x + std::round(p.px(14.0F));
            const bool closed = item.source == TabSearchItem::Source::Closed;
            draw_folder_glyph(r, pal, x, std::round(mid - (0.5F * icon)), icon,
                              closed ? 0.6F : 1.0F);
            x += icon + std::round(p.px(10.0F));
            // Right: where the tab's pane sits (a mini map of the layout), or "Closed".
            RectF cr;
            if (closed) {
                const ShapedText ct = p.shape(type::caption, tr("search_tabs.closed_chip"));
                const float chip_h = std::round(p.px(20.0F));
                const float chip_w = std::round(ct.width() + p.px(14.0F));
                cr = snap(RectF{rr.right() - p.px(10.0F) - chip_w, mid - (0.5F * chip_h), chip_w,
                                chip_h});
                r.stroke_rounded_rect(cr, 0.5F * chip_h, std::max(1.0F, std::round(p.px(1.0F))),
                                      pal.keycap_border);
                p.draw(ct, cr.x + std::round(p.px(7.0F)),
                       std::round(mid + (0.5F * ct.cap_height())), pal.text_secondary);
            } else {
                const float mw = std::round(p.px(26.0F));
                const float mh = std::round(p.px(17.0F));
                cr = snap(RectF{rr.right() - p.px(12.0F) - mw, mid - (0.5F * mh), mw, mh});
                const float gap = std::max(1.0F, std::round(p.px(1.5F)));
                for (const PaneRect& pr : minimap.panes) {
                    const RectF cell = snap(RectF{cr.x + (pr.rect.x / 1000.0F * cr.w) + gap,
                                                  cr.y + (pr.rect.y / 1000.0F * cr.h) + gap,
                                                  (pr.rect.w / 1000.0F * cr.w) - (2.0F * gap),
                                                  (pr.rect.h / 1000.0F * cr.h) - (2.0F * gap)});
                    const bool here = pr.pane == item.pane;
                    r.fill_rounded_rect(cell, p.px(1.5F),
                                        here ? pal.accent : faded(pal.keycap_border, 0.55F));
                }
            }
            float right = cr.x - std::round(p.px(10.0F));
            if (item.active) {
                const ShapedText cur = p.shape(type::caption, tr("search_tabs.current"));
                right -= cur.width();
                p.draw(cur, right, std::round(mid + (0.5F * cur.cap_height())), pal.accent);
                right -= std::round(p.px(10.0F));
            }
            const float room = right - x;
            const ShapedText title = fitter_.fit(
                p, type::row, item.title, std::min(room * 0.55F, p.px(260.0F)), Elide::Middle);
            const float baseline = std::round(mid + (0.5F * title.cap_height()));
            p.draw(title, x, baseline, closed ? pal.text_secondary : pal.text_primary);
            const float dx = x + title.width() + std::round(p.px(10.0F));
            if (right - dx > p.px(24.0F)) {
                const ShapedText detail =
                    fitter_.fit(p, type::caption, item.detail, right - dx, Elide::Middle);
                p.draw(detail, dx, baseline, pal.text_muted);
            }
            if (inside && ptr.released && popup_.pressed == WidgetId::PopupRow &&
                popup_.pressed_index == row.filtered) {
                popup_.highlight = row.filtered;
                r.set_clip(std::nullopt);
                confirm_popup(f.queue, f.intents);
                return;
            }
            y += row_h;
        }
        r.set_clip(std::nullopt);
    }
    draw_footer_hints(f, RectF{card.x, card.bottom() - foot_h, card.w, foot_h},
                      {{"\xE2\x86\x91 \xE2\x86\x93", std::string(tr("hint.move"))},
                       {"Enter", std::string(tr("hint.switch"))},
                       {"Esc", std::string(tr("hint.close"))}});
}

void WorkspaceView::draw_save_layout(const ShellFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float pad = std::round(p.px(18.0F));
    const float title_h = std::round(p.px(20.0F));
    const float field_h = std::round(p.px(38.0F));
    const float msg_h = std::round(p.px(28.0F));
    const float btn_h = std::round(p.px(32.0F));
    const float foot_h = std::round(p.px(kFooterHeight));
    const float total =
        pad + title_h + std::round(p.px(12.0F)) + field_h + msg_h + btn_h + pad + foot_h;
    const float top = std::round(std::max(p.px(56.0F), f.height * 0.18F));
    const RectF card = draw_card(f, kPromptWidth, total, top);
    if (ptr.pressed && !contains(card, ptr.x, ptr.y) && !popup_.just_opened) {
        close_popup();
        return;
    }
    float y = card.y + pad;
    const float x = card.x + pad;
    const float w = card.w - (2.0F * pad);
    draw_layout_glyph(r, x, y + p.px(1.0F), std::round(p.px(18.0F)),
                      std::max(1.0F, std::round(p.px(1.4F))), pal.accent);
    const ShapedText title = p.shape(type::popup_title, tr("layout.save_title"));
    p.draw(title, x + std::round(p.px(28.0F)),
           std::round(y + (0.5F * title_h) + (0.5F * title.cap_height())), pal.text_primary);
    y += title_h + std::round(p.px(12.0F));
    const RectF box = snap(RectF{x, y, w, field_h});
    const float rad = std::round(p.px(radius::md - 1.0F));
    r.fill_rounded_rect(box, rad, pal.surface);
    const bool error = popup_.message_error && !popup_.message.empty();
    r.stroke_rounded_rect(box, rad, std::max(1.0F, std::round(p.px(1.5F))),
                          error ? pal.danger : pal.focus_ring);
    draw_field(f, RectF{box.x + p.px(12.0F), box.y, box.w - p.px(24.0F), box.h},
               tr("layout.name_placeholder"), false);
    y += field_h;
    // Helper line: validation error, replace confirmation, or the hint.
    std::string msg = popup_.message;
    Color ink = pal.text_muted;
    if (error) {
        ink = pal.danger;
    } else if (!popup_.confirm.empty()) {
        ink = pal.text_secondary;
    } else {
        msg = substitute(tr("layout.save_hint"), "n",
                         format_count(f.workspace.layout().pane_count()));
    }
    const ShapedText m = fitter_.fit(p, type::caption, msg, w, Elide::End);
    p.draw(m, x, std::round(y + (0.5F * msg_h) + (0.5F * m.cap_height())), ink);
    y += msg_h;
    // Buttons, right aligned: Cancel, Save (or Replace).
    const bool replace = !popup_.confirm.empty();
    const ShapedText save_t = p.shape(type::button, tr(replace ? "layout.replace" : "layout.save"));
    const ShapedText cancel_t = p.shape(type::button, tr("layout.cancel"));
    const float bw_save = std::round(std::max(save_t.width() + p.px(28.0F), p.px(84.0F)));
    const float bw_cancel = std::round(std::max(cancel_t.width() + p.px(28.0F), p.px(84.0F)));
    const RectF save_r{card.right() - pad - bw_save, y, bw_save, btn_h};
    const RectF cancel_r{save_r.x - std::round(p.px(8.0F)) - bw_cancel, y, bw_cancel, btn_h};
    if (popup_button(f, cancel_r, tr("layout.cancel"), false, false, 0)) {
        close_popup();
        return;
    }
    draw_footer_hints(f, RectF{card.x, card.bottom() - foot_h, card.w, foot_h},
                      {{"Enter", std::string(tr(replace ? "layout.replace" : "layout.save"))},
                       {"Esc", std::string(tr("layout.cancel"))}});
    if (popup_button(f, save_r, tr(replace ? "layout.replace" : "layout.save"), true, replace, 1)) {
        confirm_popup(f.queue, f.intents);
    }
}

void WorkspaceView::draw_layout_picker(const ShellFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float row_h = std::round(p.px(kRowHeight));
    const float field_h = std::round(p.px(kFieldHeight));
    const float foot_h = std::round(p.px(kFooterHeight));
    const float pad = std::round(p.px(6.0F));
    const float head_h = std::round(p.px(kSectionHeight + 4.0F));
    const std::size_t n = popup_.names_filtered.size();
    const float list_h =
        n == 0 ? std::round(p.px(72.0F)) : row_h * static_cast<float>(std::min(n, kMaxRows));
    const float total = field_h + head_h + list_h + (2.0F * pad) + foot_h;
    const float top = std::round(std::max(p.px(56.0F), f.height * 0.14F));
    const RectF card = draw_card(f, kPickerWidth, total, top);
    if (ptr.pressed && !contains(card, ptr.x, ptr.y) && !popup_.just_opened) {
        close_popup();
        return;
    }
    const float inner_x = card.x + std::round(p.px(14.0F));
    draw_field(f, RectF{inner_x, card.y, card.w - std::round(p.px(28.0F)), field_h},
               tr("layout.filter_placeholder"), true);
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    r.fill_rect(RectF{card.x, card.y + field_h, card.w, hair}, pal.border);
    const ShapedText head = p.shape(
        type::section, tr(popup_.delete_mode ? "layout.delete_title" : "layout.load_title"));
    p.draw(head, card.x + pad + std::round(p.px(10.0F)),
           std::round(card.y + field_h + head_h - p.px(8.0F)),
           popup_.delete_mode ? pal.danger : pal.text_muted);

    const RectF list{card.x + pad, card.y + field_h + head_h, card.w - (2.0F * pad), list_h};
    if (n == 0) {
        const std::string text = !popup_.message.empty() ? popup_.message
                                 : popup_.names.empty()  ? std::string(tr("layout.none"))
                                                         : std::string(tr("layout.no_match"));
        const ShapedText t = fitter_.fit(p, type::row, text, list.w - p.px(24.0F), Elide::End);
        const float cy = list.center_y() - (popup_.names.empty() ? p.px(10.0F) : 0.0F);
        p.draw(t, std::round(list.center_x() - (0.5F * t.width())),
               std::round(cy + (0.5F * t.cap_height())), pal.text_secondary);
        if (popup_.names.empty() && popup_.message.empty()) {
            const std::string chord = chord_text(f.keymap, "layout.save", 0, true);
            const ShapedText hint = p.shape(type::caption, tr("layout.none_hint"));
            const float kw = chord.empty() ? 0.0F : draw_keycap(f, chord, 0.0F, 0.0F, true);
            const float tw = kw + p.px(8.0F) + hint.width();
            float hx = std::round(list.center_x() - (0.5F * tw));
            const float hy = cy + p.px(24.0F);
            if (!chord.empty()) {
                hx += draw_keycap(f, chord, hx, hy, false) + p.px(8.0F);
            }
            p.draw(hint, hx, std::round(hy + (0.5F * hint.cap_height())), pal.text_muted);
        }
    } else {
        auto first = static_cast<std::size_t>(popup_.scroll);
        if (popup_.highlight < first) {
            first = popup_.highlight;
        } else if (popup_.highlight >= first + kMaxRows) {
            first = popup_.highlight + 1 - kMaxRows;
        }
        popup_.scroll = static_cast<float>(first);
        r.set_clip(list);
        for (std::size_t k = first; k < n && k < first + kMaxRows; ++k) {
            const std::string& name = popup_.names[popup_.names_filtered[k]];
            const RectF rr = snap(
                RectF{list.x, list.y + (static_cast<float>(k - first) * row_h), list.w, row_h});
            const bool inside = contains(rr, ptr.x, ptr.y);
            if (inside && popup_.pointer_moved) {
                if (popup_.highlight != k) {
                    popup_.confirm.clear();
                }
                popup_.highlight = k;
            }
            const bool hi = popup_.highlight == k;
            const bool confirming = popup_.confirm == name;
            const float rad = std::round(p.px(radius::md - 2.0F));
            if (confirming) {
                r.fill_rounded_rect(rr, rad, pal.danger_surface);
                r.stroke_rounded_rect(rr, rad, std::max(1.0F, std::round(p.px(1.0F))),
                                      pal.danger_border);
            } else if (hi) {
                r.fill_rounded_rect(rr, rad, pal.selection);
            }
            const float mid = rr.center_y();
            const float icon = std::round(p.px(16.0F));
            const float x = rr.x + std::round(p.px(14.0F));
            draw_layout_glyph(r, x, std::round(mid - (0.5F * icon)), icon,
                              std::max(1.0F, std::round(p.px(1.3F))),
                              confirming ? pal.danger : pal.text_secondary);
            const float tx = x + icon + std::round(p.px(10.0F));
            // Delete button (hover / highlight), or the confirmation text.
            const float b = std::round(p.px(24.0F));
            const RectF del = snap(RectF{rr.right() - p.px(8.0F) - b, mid - (0.5F * b), b, b});
            float right = rr.right() - p.px(12.0F);
            if (confirming) {
                const ShapedText c = p.shape(type::caption, tr("layout.delete_confirm"));
                right -= c.width();
                p.draw(c, right, std::round(mid + (0.5F * c.cap_height())), pal.danger);
                right -= p.px(12.0F);
            } else if (hi || inside) {
                const bool over = contains(del, ptr.x, ptr.y);
                if (over) {
                    r.fill_rounded_rect(del, p.px(radius::sm), pal.control_hover);
                }
                draw_cross(r, del.center_x(), mid, p.px(4.0F), std::max(1.0F, p.px(1.4F)),
                           over ? pal.danger : pal.text_muted);
                right = del.x - p.px(8.0F);
                if (over && ptr.pressed) {
                    popup_.pressed = WidgetId::PopupDelete;
                    popup_.pressed_index = k;
                }
            }
            const ShapedText t = fitter_.fit(p, type::row, name, right - tx, Elide::Middle);
            p.draw(t, tx, std::round(mid + (0.5F * t.cap_height())),
                   confirming ? pal.danger : pal.text_primary);
            if (inside && ptr.pressed && popup_.pressed != WidgetId::PopupDelete) {
                popup_.pressed = WidgetId::PopupRow;
                popup_.pressed_index = k;
            }
            if (inside && ptr.released && popup_.pressed_index == k) {
                if (popup_.pressed == WidgetId::PopupDelete && contains(del, ptr.x, ptr.y)) {
                    popup_.highlight = k;
                    if (popup_.confirm == name) {
                        Intent in;
                        in.kind = Intent::Kind::DeleteLayout;
                        in.text = name;
                        f.intents.push_back(std::move(in));
                        popup_.confirm.clear();
                    } else {
                        popup_.confirm = name;
                    }
                } else if (popup_.pressed == WidgetId::PopupRow) {
                    popup_.highlight = k;
                    r.set_clip(std::nullopt);
                    confirm_popup(f.queue, f.intents);
                    return;
                }
            }
        }
        r.set_clip(std::nullopt);
    }
    std::vector<std::pair<std::string, std::string>> hints{
        {"\xE2\x86\x91 \xE2\x86\x93", std::string(tr("hint.move"))},
        {"Enter", std::string(tr(popup_.delete_mode ? "hint.delete" : "hint.load"))}};
    if (!popup_.delete_mode) {
        hints.emplace_back("Del", std::string(tr("hint.delete")));
    }
    hints.emplace_back("Esc", std::string(tr("hint.close")));
    draw_footer_hints(f, RectF{card.x, card.bottom() - foot_h, card.w, foot_h}, hints);
}

void WorkspaceView::draw_menu(const ShellFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float row_h = std::round(p.px(kMenuRow));
    const float sep_h = std::round(p.px(9.0F));
    const float pad = std::round(p.px(5.0F));
    float label_w = 0.0F;
    float hint_w = 0.0F;
    float h = 2.0F * pad;
    for (const MenuItem& m : popup_.menu) {
        label_w = std::max(label_w, p.shape(type::row, m.label).width());
        if (!m.hint_command.empty()) {
            hint_w = std::max(hint_w, p.shape(type::caption, m.hint_command).width());
        }
        h += row_h + (m.separator_before ? sep_h : 0.0F);
    }
    const bool list = popup_.kind == PopupKind::TabList;
    const float icon = list ? std::round(p.px(14.0F) + p.px(9.0F)) : 0.0F;
    float w = std::round(icon + label_w + (hint_w > 0.0F ? hint_w + p.px(28.0F) : 0.0F) +
                         p.px(28.0F) + (2.0F * pad));
    w = std::clamp(w, std::round(p.px(200.0F)), f.width - p.px(16.0F));
    h = std::min(h, f.height - p.px(16.0F));
    float x = list ? popup_.menu_x - w : popup_.menu_x;
    float y = popup_.menu_y;
    x = std::clamp(x, p.px(8.0F), f.width - w - p.px(8.0F));
    if (y + h > f.height - p.px(8.0F)) {
        y = std::max(p.px(8.0F), y - h);
    }
    const RectF card = snap(RectF{x, y, w, h});
    const float rad = std::round(p.px(radius::md + 1.0F));
    if ((ptr.pressed || ptr.right_pressed) && !contains(card, ptr.x, ptr.y) &&
        !popup_.just_opened) {
        close_popup();
        return;
    }
    r.drop_shadow(card, rad, p.px(22.0F), p.px(6.0F), pal.shadow);
    r.fill_rounded_rect(card, rad, pal.surface_raised);
    r.stroke_rounded_rect(card, rad, std::max(1.0F, std::round(p.px(1.0F))), pal.border);
    float yy = card.y + pad;
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    r.set_clip(card);
    for (std::size_t i = 0; i < popup_.menu.size(); ++i) {
        const MenuItem& m = popup_.menu[i];
        if (m.separator_before) {
            r.fill_rect(
                RectF{card.x + pad, std::round(yy + (0.5F * sep_h)), card.w - (2.0F * pad), hair},
                pal.border);
            yy += sep_h;
        }
        const RectF rr = snap(RectF{card.x + pad, yy, card.w - (2.0F * pad), row_h});
        const bool inside = contains(rr, ptr.x, ptr.y);
        if (inside && popup_.pointer_moved && m.enabled) {
            popup_.highlight = i;
        }
        if (popup_.highlight == i && m.enabled) {
            r.fill_rounded_rect(rr, std::round(p.px(radius::sm)), pal.selection);
        }
        float tx = rr.x + std::round(p.px(10.0F));
        if (list) {
            const float ic = std::round(p.px(14.0F));
            draw_folder_glyph(r, pal, tx, std::round(rr.center_y() - (0.5F * ic)), ic, 1.0F);
            tx += ic + std::round(p.px(9.0F));
        }
        const ShapedText t = fitter_.fit(p, type::row, m.label,
                                         rr.right() - tx - hint_w - p.px(20.0F), Elide::Middle);
        const float baseline = std::round(rr.center_y() + (0.5F * t.cap_height()));
        p.draw(t, tx, baseline, m.enabled ? pal.text_primary : faded(pal.text_muted, 0.8F));
        if (!m.hint_command.empty()) {
            const ShapedText hk = p.shape(type::caption, m.hint_command);
            p.draw(hk, rr.right() - p.px(10.0F) - hk.width(), baseline, pal.text_muted);
        }
        if (inside && ptr.pressed) {
            popup_.pressed = WidgetId::PopupRow;
            popup_.pressed_index = i;
        }
        if (inside && ptr.released && m.enabled && popup_.pressed == WidgetId::PopupRow &&
            popup_.pressed_index == i) {
            popup_.highlight = i;
            r.set_clip(std::nullopt);
            confirm_popup(f.queue, f.intents);
            return;
        }
        yy += row_h;
    }
    r.set_clip(std::nullopt);
}

} // namespace navigator::ui
