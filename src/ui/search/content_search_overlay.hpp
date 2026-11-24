// src/ui/search/content_search_overlay.hpp
//
// Phase 10 cancellable content search overlay (Ctrl+Shift+G). Runs search::ContentSearchSession
// off-thread and renders live streaming file+line matches. Clicking or pressing Enter on a match
// returns its file path so App navigates to its parent directory and selects it.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <navigator/content_search.hpp>
#include <navigator/types.hpp>

#include "render/renderer.hpp"
#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/text_fit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

class ContentSearchOverlay {
public:
    void open(const Path& root);
    void close();
    [[nodiscard]] bool open() const noexcept { return open_; }
    [[nodiscard]] bool active() const noexcept { return session_.active(); }

    // Polls background search worker; returns true if new matches or stats arrived.
    bool poll();

    // Handles keyboard input while open. Returns a selected file Path if Enter activated a match.
    std::optional<Path> on_key(Key key, Modifiers mods);
    void on_text(std::string_view utf8);

    // Draws the centered overlay. Returns a selected file Path if a match row was clicked.
    std::optional<Path> draw(const Painter& p, const Palette& pal, const PointerInput& ptr,
                             float width, float height);

private:
    void restart_search();

    bool open_ = false;
    Path root_;
    TextEdit query_;
    float query_scroll_ = 0.0F;
    bool case_sensitive_ = false;
    std::vector<search::ContentMatch> matches_;
    search::ContentSearchStats stats_;
    std::size_t cursor_ = 0;
    std::size_t scroll_row_ = 0;
    search::ContentSearchSession session_;
    TextFitter fitter_;
};

} // namespace navigator::ui
