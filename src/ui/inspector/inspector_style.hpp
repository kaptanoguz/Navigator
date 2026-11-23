// src/ui/inspector/inspector_style.hpp
//
// Phase 6 Inspector colours that carry a contrast requirement, pure so tests pin them down:
// the text view's line-number gutter (design review Phase 6 optional 12: >= 4.5:1 in both
// themes; the band is a faint tint of the raised surface).
// Thread-safety: pure functions.
#pragma once

#include "theme/theme.hpp"

namespace navigator::ui::inspector {

// The gutter band, composited over Palette::surface_raised.
[[nodiscard]] inline Color gutter_fill(const Palette& pal) noexcept {
    return pal.text_muted.with_alpha(0.055F);
}
// Opaque colour of the band (what the line numbers sit on).
[[nodiscard]] inline Color gutter_background(const Palette& pal) noexcept {
    const Color f = gutter_fill(pal);
    return mix(pal.surface_raised, f.with_alpha(1.0F), f.a);
}
// Line numbers: the metadata key colour, one step above text_muted.
[[nodiscard]] inline Color gutter_ink(const Palette& pal) noexcept { return pal.text_secondary; }

} // namespace navigator::ui::inspector
