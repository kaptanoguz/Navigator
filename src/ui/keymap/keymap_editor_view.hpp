// src/ui/keymap/keymap_editor_view.hpp
//
// Phase 8 Settings overlay ("Keyboard" page; Ctrl+, and Ctrl+K Ctrl+S):
//   - lists every bindable command + builtin_command_variants() with live shortcuts;
//   - search box filters by title, command name, category, or shortcut text;
//   - click or Enter enters shortcut capture mode (1..kMaxSequenceLength chords, including keypad
//     and Menu key; Enter commits, Escape cancels);
//   - shows BindingConflict ("Replace" button) and BindingWarning badges from check_binding();
//   - per-command Reset, sequence timeout stepper (200..5000 ms), Export (keymap.export), and
//     Reset Defaults (keymap.reset_defaults -> confirmation sheet).
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/appearance.hpp>
#include <navigator/commands.hpp>
#include <navigator/keymap_file.hpp>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

struct KeymapEditorEvent {
    bool keymap_dirty = false;          // caller rebuilds Keymap, resets SequenceMatcher & saves
    bool appearance_dirty = false;      // Phase 9B: caller applies & persists AppearanceSettings
    std::optional<Command> enqueue_cmd; // e.g. ResetKeymap or ExportKeymap
};

class KeymapSettingsOverlay {
public:
    enum class Page : std::uint8_t {
        Appearance = 0,
        Keyboard = 1,
    };

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] Page page() const noexcept { return page_; }
    [[nodiscard]] bool is_capturing() const noexcept {
        return open_ && page_ == Page::Keyboard && capturing_;
    }
    [[nodiscard]] bool wants_text_input() const noexcept {
        return open_ && page_ == Page::Keyboard && !capturing_;
    }
    [[nodiscard]] TextEdit* active_field() noexcept {
        return (open_ && page_ == Page::Keyboard && !capturing_) ? &search_ : nullptr;
    }

    void open(const CommandRegistry& registry, const Keymap& effective,
              Page page = Page::Keyboard);
    void close() noexcept;

    // Refresh displayed shortcuts after the effective Keymap changes.
    void refresh(const CommandRegistry& registry, const Keymap& effective);

    void search_changed();

    // Handles a key press while the overlay is open (both normal navigation and capture mode).
    KeymapEditorEvent handle_key(KeyChord chord, KeymapFile& file, const Keymap& effective,
                                 const Keymap& defaults, const CommandRegistry& registry);

    KeymapEditorEvent draw(const Painter& p, const Palette& pal, const RectF& viewport,
                           const PointerInput& pointer, KeymapFile& file, const Keymap& effective,
                           const Keymap& defaults, const CommandRegistry& registry,
                           appearance::AppearanceSettings& appearance,
                           const std::vector<appearance::ThemeScheme>& custom_themes);

private:
    struct Row {
        std::string command;
        std::uint64_t arg = 0;
        std::string title;
        std::string category;
        std::string shortcut;
    };

    void rebuild_rows(const CommandRegistry& registry, const Keymap& effective);
    void refilter();
    void begin_capture(std::size_t filtered_idx);
    void cancel_capture() noexcept;
    KeymapEditorEvent commit_capture(KeymapFile& file, const Keymap& effective,
                                     const Keymap& defaults, const CommandRegistry& registry,
                                     bool replace_conflicts);

    bool open_ = false;
    Page page_ = Page::Appearance;
    std::vector<Row> rows_;
    std::vector<std::size_t> filtered_;
    std::size_t selected_ = 0;
    std::size_t scroll_ = 0;

    // Capture state.
    bool capturing_ = false;
    std::size_t capture_row_ = 0; // index into rows_
    KeySequence captured_seq_;
    std::optional<BindingCheck> check_;

    TextEdit search_;
    float field_scroll_ = 0.0F;
};

} // namespace navigator::ui
