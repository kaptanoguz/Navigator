# Phase 0 design review

Reviewer: design-reviewer. Branch `phase-0` at `6f050c1`.
Evidence: 1x and 2x screenshots in both themes and both languages, software renderer, Wayland
(weston) and live key-toggle captures. I also read `src/ui/theme`, `src/ui/shell/scene.cpp`,
`src/ui/shell/keyboard.cpp` and the pacing notes in `app.cpp`.

## Verdict

**Ship Phase 0.** I found no blocking items. The shell is calm and deliberate, and nothing in it
looks like default Dear ImGui. It has no File Pilot name, logo or artwork. The wordmark is the
plain "Navigator" text.

## What works

- **Hierarchy.** There are three clear levels. The wordmark is 60dp SemiBold with -0.028em
  tracking. The tagline is 17dp Regular in secondary text. The hint row is 12dp. The 28x3dp
  accent bar gives the wordmark a focal point without acting as a logo. Content sits at 45%
  of the window height (an optical centre), and spacing uses a 4dp scale.
- **Contrast.** I computed WCAG ratios from the palette. Primary text is far above AA. Secondary
  text is 7.98:1 in dark and 6.5:1 in light. The accent is 8.0:1 in dark and 4.83:1 in light.
- **Keycaps.** Radius is 5dp and height is 24dp. A 1px darker bottom edge gives depth.
  Legends are centred, and the legend and label baselines line up across the row. Spacing
  holds at 2x.
- **FPS overlay.** Figures are tabular. The value column reserves room for "000.0 ms", so the
  panel does not jitter. Values are right-aligned. The panel has a 12dp radius, a hairline
  border and a soft shadow.
- **HiDPI.** The 2x render is sharp. Glyphs are rasterised at physical size, not upscaled.
  Hairlines stay 1px.
- **Turkish.** ç, ı, ğ, ş, ö and Ç render correctly with HarfBuzz at 1x and 2x. The Turkish
  strings are a little longer, and the row recentres cleanly.
- **Background.** The radial glow uses TPDF dither. I stretched the dark capture to check for
  banding and found none.
- **Feels immediate.** When idle, the app waits on SDL events and does not spin. The theme
  crossfade is 180ms and blends in linear light. It can be turned off with
  `NAVIGATOR_NO_ANIMATIONS`. Vsync drives pacing. The live capture showed 120 FPS at 8.3ms.
  The 3 to 11 FPS figures in the xvfb/llvmpipe screenshots come from the headless renderer.
  They are not a product problem.
- **Keyboard paths.** These match `docs/keymap.md`: Ctrl+Q, F12, Ctrl+Shift+L and window close
  all go through the command registry. Keys are matched by keycode first, with a scancode
  fallback only for non-Latin keycodes. This means Turkish Q and F layouts both resolve the
  printed L and Q without stealing other keys.

## BLOCKING

None.

## OPTIONAL (Phase 0 polish, any order)

1. **Muted text contrast is below AA.** `text_muted` is 3.51:1 on the light background, 3.8:1 on
   white, and 3.7:1 on the dark raised surface. In Phase 0 only the FPS overlay labels use it,
   which is acceptable for a debug panel. Fix it before Phase 2, though: see P2-1. Suggested
   values: light around `#6A7080` and dark around `#7F8697`. Both reach about 4.5:1 on their
   surfaces.
2. **The FPS overlay is on by default.** `include/navigator/ui.hpp` sets `show_fps = true`.
   While it is visible the loop renders every frame, so a default launch never idles. The
   keymap calls it a "Debug/perf overlay". Default it to off, or to on only in Debug builds.
   Keep `--fps` for QA.
3. **Renderer value.** The overlay shows `gpu` or `software` in lowercase under a capitalised
   label. "gpu" says little. Show the backend, such as "Vulkan" or "OpenGL 3.3", or a
   translated "Software".
4. **Turkish frame-time label.** "Kare" alone is ambiguous: it also means "square". Use
   "Kare süresi".
5. **Keycap legend vs action label.** Both use `text_secondary`, so they have the same visual
   weight. Make the legends one step quieter, or make the labels one step stronger, so the
   eye reads the action first and the chord second.
6. **Dark keycap separation.** The border is about 1.4:1 against the background. This is
   elegant on a good panel, but the chips may disappear on low-contrast or dimmed laptop
   screens. Consider lifting the dark `border` one step, to about `#323845`.

## Notes for Phase 2 (real file list)

- **P2-1.** Fix the muted contrast (optional item 1) before secondary columns ship: size, date,
  type and path breadcrumbs. Those are body text and must reach AA 4.5:1 in both themes. Add a
  unit test that asserts the contrast ratios of the palette pairs. `contrast_ratio()` already
  exists.
- **P2-2. Density.** The 4dp scale and 12/13/14dp text are a good base. File Pilot's documented
  UX is dense and keyboard-first. Plan for a compact row of about 22–24dp, plus a comfortable
  option. Use tabular figures for size and date columns. Truncate names in the middle, keeping
  the extension. Never let the selection highlight rely on colour alone.
- **P2-3. Empty states.** Phase 0 is an empty state. Reuse its voice for these: an empty folder,
  no search matches, permission denied, and an unmounted volume. Each needs one quiet line plus
  one keycap hint for the next action. Do not use illustrations.
- **P2-4. Focus and keyboard.** Make focus rings explicit: 2px accent, visible in both themes.
  Add type-ahead and arrow, Home/End and PageUp/PageDown navigation, and give every new action
  a row in `docs/keymap.md` and the palette. Re-run the Turkish-layout chord check whenever
  bindings use letters like I/İ, Ğ, Ü, Ş or Ö.
- **P2-5. Immediacy.** Paint the first frame of a directory before metadata arrives, then
  stream rows in without layout shift. Keep column widths stable while listing. Keep the
  event-driven idle loop, and do not let a spinner force continuous redraw.
- **P2-6. Turkish casing.** Sorting, case-insensitive filtering and title casing must use
  locale-aware rules: i/İ and ı/I. Do not use ASCII `tolower`.
- **Housekeeping.** `CLAUDE.md` refers to `docs/features.md`, but that file does not exist yet.
  The coordinator should add it before UX work that cites File Pilot behaviour.
