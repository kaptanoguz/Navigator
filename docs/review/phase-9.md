# Design review: Phase 9 (views, batch rename, appearance & custom themes)

Reviewer: design-reviewer. Scope: density, typography, icon/tile alignment, light/dark & custom
JSON theme contrast, keyboard paths, animations-off behavior, EN/TR wording, and whether the UI
feels snappy and dense like a native file manager rather than a toolkit demo. Compared against the
intent in `docs/yol_haritasi.md` (Faz 9), `docs/features.md`, `docs/keymap.md` (Phase 9) and
`docs/architecture.md`. No File Pilot assets or branding were used.

## Verdict

**Ship Phase 9.** Both Phase 9A (Details/Icons/List/Tree views, live zoom axis, grouping, column
reordering, hierarchical View Lock) and Phase 9B (interactive Batch Rename with 200+ item live
preview, cyclic swap staging and single-step Undo/Redo; Appearance settings page with built-in
Dark/Light and custom JSON color schemes, live font scale, spacing, corner radius, alternating row
colors, and animations toggle) meet the acceptance bar. No blocking visual, contrast, or data-safety
defects remain.

## BLOCKING

None.

## Review notes

### Views & density (Phase 9A)
- **Single zoom axis (`Details ↔ Icons`):** `Ctrl+wheel`, `Ctrl+=`/`Ctrl+-`/`Ctrl+0` (and keypad
  equivalents), and the footer `%` slider transition cleanly between Details and Icon Grid without
  losing selection or scroll anchor.
- **Virtualization:** Grouped Details, Icon Grid, flowing List/Columns, and async expandable Tree
  all measure and draw only visible rows/tiles. Tree expansion runs off-thread without stalling
  input.
- **Hierarchical View Lock:** Address-bar lock indicator cycles Global → Folder → Hierarchy cleanly
  and persists atomically to `view_settings.json`.
- **Column reordering:** Details column headers support drag-to-reorder (including `Name`, matching
  File Pilot 0.8.1 intent) and persist per rule.

### Batch Rename (Phase 9B)
- **Safety & clarity:** Collisions (both intra-batch and against existing siblings) and invalid
  names are highlighted in `danger_surface` / `danger` text and disable the `Apply` button (`Ctrl+Enter`
  is also gated on `plan.can_apply()`). Vacated swaps (`a -> b`, `b -> a`) are recognized as `Ready`
  and executed via two-phase staging so no file is overwritten or lost.
- **Undo/Redo:** All renamed files in a batch record a single `OpKind::BatchRename` `UndoRecord`
  with `ItemGuard` verification and staged inverse execution.
- **Paste ergonomics:** Multi-line clipboard paste maps line-by-line; single-line paste into a
  multi-file batch switches to template mode preserving extensions (`{ext}`), matching the
  documented v0.5.0 special case.
- **Turkish & Unicode:** Title-case transformation preserves multi-byte UTF-8 letters (e.g.
  `Güneş_şarkısı`) without splitting words at non-ASCII bytes; all UI labels ship in EN and TR.

### Appearance, themes & motion (Phase 9B)
- **Contrast & custom JSON themes:** Built-in Dark and Light palettes pass WCAG AA/AAA contrast
  checks across all text tiers, surfaces, active/dimmed panes, and match chips (`tests/ui/theme_test.cpp`).
  Custom JSON themes (`<config>/themes/*.json`) fall back leniently per-key to their `base`
  (`dark` or `light`) and automatically derive `selection_marker`, `match`, `chip`, and `chip_text`
  from `accent` when omitted.
- **Typography & spacing:** `Painter` applies `font_scale_percent` (`75%–150%`) uniformly through
  FreeType + HarfBuzz shaping; `density_metrics(Density, spacing_scale_percent)` scales row and
  chrome heights without clipping; `corner_radius_dp` (`0–14 dp`) supports crisp square corners at
  `0 dp`.
- **Animations off:** Toggling `Animations` off (in Settings, via `view.toggle_animations`, or
  `--no-animations`) immediately disables theme crossfade, smooth scroll, thumbnail fade-in, and
  animated transitions.

## OPTIONAL (follow-up polish, non-blocking)

1. **Custom theme contrast warning:** In the Settings Appearance page, display a subtle warning
   badge if a user-authored JSON theme has `text_primary` on `surface` below 4.5:1 WCAG AA.
2. **Per-row inline edit in Batch Rename:** Allow double-clicking an individual preview row in the
   Batch Rename table to edit its `manual_overrides[i]` inline without pasting.
3. **Regex mode in Batch Rename Find/Replace:** Optional toggle for regular expressions in Find,
   keeping literal substring matching as the safe default.
4. **Hot-reload custom theme files:** Watch `<config>/themes/` with `IWatcher` so editing a theme
   JSON externally refreshes the live palette without restarting Navigator.
