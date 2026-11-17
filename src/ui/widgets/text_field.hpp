// src/ui/widgets/text_field.hpp
//
// Draws a TextEdit (Phase 4): text, selection highlight and a steady caret (no blink, so an
// idle field does not redraw), scrolled horizontally so the caret stays visible. Shared by the
// address bar, the GoTo box, the Phase 3 name prompts and the bookmark rename field.
// Thread-safety: UI thread only.
#pragma once

#include <string_view>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"
#include "widgets/text_edit.hpp"

namespace navigator::ui {

struct TextFieldStyle {
    const TextStyle* text = nullptr; // default type::popup_field
    float caret_height_dp = 18.0F;
    bool show_caret = true;
};

// Draws `edit` inside `box` (px; the text is vertically centred). `scroll` (px) is the field's
// persistent horizontal offset, updated to keep the caret in view. Returns the caret x (px).
float draw_text_field(const Painter& p, const Palette& pal, const TextEdit& edit, const RectF& box,
                      std::string_view placeholder, float& scroll,
                      const TextFieldStyle& style = {});

} // namespace navigator::ui
