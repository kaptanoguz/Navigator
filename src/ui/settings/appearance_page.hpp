// src/ui/settings/appearance_page.hpp
//
// Phase 9B Settings "Appearance" page:
//   - Theme selector (built-in Dark, Light, plus custom JSON schemes from <config>/themes/*.json
//     with live color swatches);
//   - Density toggle (Compact / Comfortable);
//   - Font scale stepper (75%..150%), Spacing scale stepper (75%..150%), Corner radius stepper
//     (0..14 dp);
//   - Animations toggle (live; turns off crossfade, smooth scroll, fades) & Alternating row colors
//     (zebra) toggle;
//   - Reset to defaults button.
// Thread-safety: UI thread only.
#pragma once

#include <vector>

#include <navigator/appearance.hpp>

#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

// Draws the Appearance settings page inside `body` (px). Mutates `settings` in place when the user
// clicks a control and returns true if `settings` changed (caller applies live and persists).
bool draw_appearance_page(const Painter& p, const Palette& pal, const RectF& body,
                          const PointerInput& pointer, appearance::AppearanceSettings& settings,
                          const std::vector<appearance::ThemeScheme>& custom_themes);

} // namespace navigator::ui
