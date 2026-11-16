// src/ui/widgets/icons.hpp
//
// Small glyphs drawn with renderer primitives (no icon theme yet): entry glyphs for the Name
// column (folder, file with a type badge, link badge), toolbar glyphs (chevrons, up arrow, theme
// switch) and notice glyphs. All sizes are in physical pixels; callers pass painter.px(...).
// Thread-safety: UI thread only.
#pragma once

#include <navigator/fs.hpp>
#include <navigator/ui.hpp>

#include "render/renderer.hpp"
#include "theme/theme.hpp"

namespace navigator::ui {

enum class Direction : std::uint8_t { Left, Right, Up, Down };

// Entry glyph in a `size` x `size` box with its top-left at (x, y). `dim` < 1 fades it
// (unreadable folders, hidden entries).
void draw_entry_glyph(Renderer& r, const Palette& pal, const fs::DirEntry& entry, float x, float y,
                      float size, float dim, float scale);

// Open chevron centred at (cx, cy); `size` is the arm length.
void draw_chevron(Renderer& r, float cx, float cy, float size, Direction dir, float thickness,
                  const Color& color);

// Arrow pointing up (shaft + head), `size` tall, centred at (cx, cy).
void draw_arrow_up(Renderer& r, float cx, float cy, float size, float thickness,
                   const Color& color);

// Half-moon/sun theme glyph of radius `radius`: shows the theme a click switches to.
void draw_theme_glyph(Renderer& r, float cx, float cy, float radius, Theme current,
                      const Color& color, const Color& background);

// Filled circle with an exclamation mark.
void draw_alert_glyph(Renderer& r, float cx, float cy, float radius, const Color& color,
                      const Color& mark, float scale);

// A cross ("x") of half-width `size`.
void draw_cross(Renderer& r, float cx, float cy, float size, float thickness, const Color& color);

} // namespace navigator::ui
