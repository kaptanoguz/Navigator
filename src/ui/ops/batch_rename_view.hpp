// src/ui/ops/batch_rename_view.hpp
//
// Phase 9B interactive Batch Rename overlay:
//   - template field ({name}, {stem}, {ext}, {counter:3}, {date}, {uid:6}, {parent}) + quick token
//     chips;
//   - find & replace fields + Case transform cycle + Trim whitespace + Case-sensitive toggles;
//   - Paste names (single-line template special case & multi-line overrides via
//     ops::interpret_batch_paste);
//   - virtualized live preview table (handles 200+ items at 60 FPS) showing current -> proposed
//     name, collision (internal & existing) and invalid-name badges;
//   - Apply returns an ops::OpRequest (OpKind::BatchRename) for the App to submit at frame end.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/batch_rename.hpp>
#include <navigator/commands.hpp>
#include <navigator/ops.hpp>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

struct BatchRenameEvent {
    std::optional<ops::OpRequest> apply_request; // set when user clicks Apply / presses Ctrl+Enter
    bool request_paste_clipboard = false;        // caller reads clipboard and calls paste_text()
};

class BatchRenameOverlay {
public:
    enum class FocusField : std::uint8_t {
        Template = 0,
        Find = 1,
        Replace = 2,
    };

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] bool wants_text_input() const noexcept { return open_; }
    [[nodiscard]] TextEdit* active_field() noexcept;

    void open(std::vector<ops::BatchRenameItem> items,
              std::vector<std::string> existing_sibling_names);
    void close() noexcept;

    // Called when the active TextEdit's text changes.
    void field_changed();

    // Applies clipboard text via ops::interpret_batch_paste (single-line or multi-line).
    void paste_text(std::string_view clipboard_text);

    // Pure accessors for tests / diagnostics.
    [[nodiscard]] const ops::BatchRenamePlan& plan() const noexcept { return plan_; }
    [[nodiscard]] const ops::BatchRenameRule& rule() const noexcept { return rule_; }
    [[nodiscard]] std::size_t item_count() const noexcept { return items_.size(); }

    BatchRenameEvent handle_key(KeyChord chord);

    BatchRenameEvent draw(const Painter& p, const Palette& pal, const RectF& viewport,
                          const PointerInput& pointer);

private:
    void rebuild_plan();
    void insert_token(std::string_view token);
    void reset_rule();

    bool open_ = false;
    std::vector<ops::BatchRenameItem> items_;
    std::vector<std::string> siblings_;
    std::vector<std::string> manual_overrides_;
    ops::BatchRenameRule rule_;
    ops::BatchRenamePlan plan_;

    FocusField focus_ = FocusField::Template;
    TextEdit template_edit_;
    TextEdit find_edit_;
    TextEdit replace_edit_;
    float template_scroll_ = 0.0F;
    float find_scroll_ = 0.0F;
    float replace_scroll_ = 0.0F;

    std::size_t scroll_ = 0;
};

} // namespace navigator::ui
