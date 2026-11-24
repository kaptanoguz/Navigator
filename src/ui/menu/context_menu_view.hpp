// src/ui/menu/context_menu_view.hpp
//
// Phase 8 Searchable Context Menu widget:
//   - opens from right-click OR menu.show_context (Menu, Shift+F10);
//   - search field at top (type-ahead: printable characters filter live via menu::filter());
//   - pin/unpin toggle on pinnable rows (updates menu::Pins and re-arranges live);
//   - right-aligned live shortcuts via shortcut_text;
//   - destructive items last & separated in danger colour, never default-highlighted (M2/M5);
//   - Open With submenu populated asynchronously from openwith::list_handlers().
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/menu.hpp>
#include <navigator/openwith.hpp>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

struct MenuAction {
    enum class Kind : std::uint8_t { Command, OpenWith, TogglePin };
    Kind kind = Kind::Command;
    Command command;
    std::string handler_id; // OpenWith
    std::string pin_id;     // TogglePin
};

class ContextMenuOverlay {
public:
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] bool wants_text_input() const noexcept { return open_; }
    [[nodiscard]] TextEdit* active_field() noexcept { return open_ ? &query_ : nullptr; }

    void open(menu::MenuKind kind, const CommandRegistry& registry, const Keymap& keymap,
              const CommandContext& context, const menu::Target& target, const menu::Pins& pins,
              float anchor_x, float anchor_y, bool has_open_with_targets);

    void close() noexcept;

    // Called when async Open With handlers arrive for the open menu.
    void set_open_with_handlers(const std::vector<openwith::Handler>& handlers);

    // Re-arranges the menu after a pin toggle without closing it.
    void reapply_pins(const menu::Pins& pins);

    void query_changed();
    void set_query(std::string_view text);

    std::optional<MenuAction> handle_key(KeyChord chord);

    std::optional<MenuAction> draw(const Painter& p, const Palette& pal, const RectF& viewport,
                                   const PointerInput& pointer);

    [[nodiscard]] const std::vector<menu::Item>& arranged_items() const noexcept {
        return arranged_;
    }
    [[nodiscard]] const menu::FilterResult& filter_result() const noexcept { return filtered_; }

private:
    void rebuild_filter();
    void move_highlight(int delta);
    std::optional<MenuAction> activate_highlighted();

    bool open_ = false;
    float anchor_x_ = 0.0F;
    float anchor_y_ = 0.0F;
    std::vector<menu::Item> raw_;
    std::vector<menu::Item> arranged_;
    menu::FilterResult filtered_;
    std::optional<std::size_t> highlighted_; // index into arranged_ (unfiltered) or filtered_.rows
    bool submenu_open_ = false;
    std::size_t submenu_index_ = 0;
    std::optional<std::size_t> sub_highlighted_;

    TextEdit query_;
    float field_scroll_ = 0.0F;
};

} // namespace navigator::ui
