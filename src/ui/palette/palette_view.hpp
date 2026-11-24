// src/ui/palette/palette_view.hpp
//
// Phase 8 Command Palette overlay (Ctrl+Shift+P). Immediate-mode card drawn over the workspace:
//   - captures CommandContext BEFORE taking focus;
//   - lists every palette-visible command + builtin_command_variants() + usable aliases;
//   - ranks via navigator::palette::rank() (fuzzy title/name/category/description/alias, recents,
//     safe-default rule P4);
//   - supports secondary pickers for ArgKind::OpId and ArgKind::BookmarkRow (P5);
//   - never executes commands directly: returns a PaletteAction for App to enqueue on CommandQueue
//     and record in palette::History when recordable.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/keymap_file.hpp>
#include <navigator/palette.hpp>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

struct PickerOption {
    std::uint64_t arg = 0;
    std::string label;
    std::string detail;
};

struct PaletteAction {
    Command command;
    std::string history_key;
    bool record_history = false;
};

class PaletteOverlay {
public:
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] bool wants_text_input() const noexcept { return open_; }
    [[nodiscard]] TextEdit* active_field() noexcept { return open_ ? &query_ : nullptr; }

    // Opens the palette, capturing `context`, building entries from `registry`, `keymap`,
    // `aliases`, and optional live undo/redo labels.
    void open(const CommandRegistry& registry, const Keymap& keymap, Span<const Alias> aliases,
              const palette::History& history, const CommandContext& context,
              const std::string& undo_label, const std::string& redo_label,
              std::vector<PickerOption> running_ops, std::vector<PickerOption> bookmarks);

    void close() noexcept;

    // Called by App when query_ text changes (SDL text input or editing key).
    void query_changed(const palette::History& history);

    // Keyboard navigation & activation inside the palette. Returns an action when an entry is
    // chosen (caller enqueues it and closes the palette).
    std::optional<PaletteAction> handle_key(KeyChord chord, const palette::History& history);

    // Draws the centered overlay card and processes mouse hover/click/wheel.
    std::optional<PaletteAction> draw(const Painter& p, const Palette& pal, const RectF& viewport,
                                      const PointerInput& pointer, const palette::History& history);

    // For tests / debug scripts: set query text and re-rank.
    void set_query(std::string_view text, const palette::History& history);
    [[nodiscard]] const palette::SearchResult& results() const noexcept { return results_; }
    [[nodiscard]] const std::vector<palette::Entry>& entries() const noexcept { return entries_; }

private:
    enum class Mode : std::uint8_t { Commands, PickOp, PickBookmark };

    void rerank(const palette::History& history);
    void ensure_visible() noexcept;
    std::optional<PaletteAction> activate_selected();
    std::optional<PaletteAction> activate_command_hit(std::size_t hit_index);

    bool open_ = false;
    Mode mode_ = Mode::Commands;
    CommandContext context_;
    std::vector<palette::Entry> entries_;
    palette::SearchResult results_;
    std::optional<std::size_t> selected_;
    std::size_t scroll_ = 0;

    // Secondary picker state (OpId / BookmarkRow).
    Command pending_picker_cmd_;
    std::vector<PickerOption> running_ops_;
    std::vector<PickerOption> bookmarks_;
    std::vector<std::size_t> filtered_picker_;

    TextEdit query_;
    float field_scroll_ = 0.0F;
};

} // namespace navigator::ui
