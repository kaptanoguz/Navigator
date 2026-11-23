// src/ui/widgets/tooltip.hpp
//
// The tooltip bubble shared by the title bar (BrowserView) and the Phase 6 Inspector: caption
// text on the raised surface with the popup border and a soft shadow, centred under `anchor` and
// kept inside `bounds`. Draw it last so it covers the content.
// Thread-safety: UI thread only.
#pragma once

#include <string_view>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"

namespace navigator::ui {

void draw_tooltip_bubble(const Painter& p, const Palette& pal, std::string_view text,
                         const RectF& anchor, const RectF& bounds);

} // namespace navigator::ui
