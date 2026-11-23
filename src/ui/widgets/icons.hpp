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

// Half-moon/sun theme glyph of radius `radius`: shows the theme a click switches to.
void draw_theme_glyph(Renderer& r, float cx, float cy, float radius, Theme current,
                      const Color& color, const Color& background);

// Filled circle with an exclamation mark.
void draw_alert_glyph(Renderer& r, float cx, float cy, float radius, const Color& color,
                      const Color& mark, float scale);

// Phase 3 glyphs.
// Folder glyph alone (tab labels, Search Tabs rows) in a `size` box at (x, y).
void draw_folder_glyph(Renderer& r, const Palette& pal, float x, float y, float size, float dim);
// "+" of half-width `size`.
void draw_plus(Renderer& r, float cx, float cy, float size, float thickness, const Color& color);
// Magnifier of radius `radius` (lens) with a handle; `background` paints the lens hole.
void draw_search_glyph(Renderer& r, float cx, float cy, float radius, float thickness,
                       const Color& color, const Color& background);
// Split-layout glyph (a frame with one vertical and one half horizontal divider), `size` box.
void draw_layout_glyph(Renderer& r, float x, float y, float size, float thickness,
                       const Color& color);

// Phase 4: monochrome line icons (sidebar rows, address bar buttons, GoTo rows), drawn on a
// 16-unit grid in a `size` x `size` box at (x, y) with strokes of `t` px. `background` paints
// cut-outs (lock body over its shackle); `filled` draws the solid variant (Star: bookmarked).
enum class LineIcon : std::uint8_t {
    Folder,
    Clock,
    Home,
    Desktop,
    Documents,
    Downloads,
    Music,
    Pictures,
    Videos,
    Drive,
    Removable,
    Network,
    Star,
    Lock,
    Sidebar,
    Unplugged, // offline volume (bookmark / recent badge)
    // Phase 5.
    Funnel,  // types popup button
    Flatten, // flatten (search.flatten) toggle
    Stop,    // stop a running search
};
void draw_line_icon(Renderer& r, LineIcon icon, float x, float y, float size, float t,
                    const Color& color, const Color& background, bool filled = false);

// A cross ("x") of half-width `size`.
void draw_cross(Renderer& r, float cx, float cy, float size, float thickness, const Color& color);

} // namespace navigator::ui
