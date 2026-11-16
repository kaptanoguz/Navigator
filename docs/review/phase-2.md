# Phase 2 design review

Reviewer: design-reviewer. Branch `phase-2` at `bcff5ba`.

Evidence: the fixture, empty and permission-denied captures in dark and light themes, at 1x and
2x, in English and Turkish. I also rendered these extra captures under xvfb:

- an unfocused window with a multi-selection
- a 1.5x fractional scale
- an unreadable start folder, where the breadcrumb collapses
- a folder that holds only hidden items

I read `src/ui/theme/theme.{hpp,cpp}`, `src/ui/browser/browser_{view,chrome,model}.cpp`,
`src/ui/i18n/i18n.cpp`, `tests/ui/theme_test.cpp` and `docs/keymap.md`. I checked the intent
against the Phase 2 rows in `docs/features.md` ("Views and list").

## Verdict

**Ship Phase 2.** I found no blocking items. The Details browser is calm and legible, and it
reads as one product with the Phase 0 shell. It has no File Pilot name, logo or artwork. The
main weakness is density. It is a premium-feel item to fix next, not a quality blocker (see O1).

## What works

- **Selection states do not rely on colour alone.**
  - Hover is a neutral tint.
  - Selection adds an accent tint and a 3dp leading marker bar.
  - The cursor is a 1px accent outline. On a selected row the outline drops to 55% alpha, so
    cursor and selection stay distinct.
  - Contiguous selected rows merge into one shape instead of a stack of pills.
  - When the window loses focus, the selection turns grey and keeps its marker, and the cursor
    ring is hidden. This matches the native convention.
- **Contrast.** P2-1 from the Phase 0 review is done. `text_muted` is now `#868D9C` (dark) and
  `#676D7A` (light). `theme_test.cpp` asserts that every text token reaches at least 4.7:1 on
  every surface it sits on, in both themes. Secondary columns, headers and the status bar all
  pass AA.
- **Columns.**
  - Size is right-aligned under a right-aligned header, and it uses tabular figures (HarfBuzz
    `tnum`).
  - Type and Modified are left-aligned.
  - Long names are truncated in the middle and keep the extension, as P2-2 asked.
  - Today and Yesterday are relative. Other dates are absolute and localised: "10 Kas 2026", and
    the Turkish decimal comma in "88,1 kB".
- **Icons.** The icons are small and consistent: tinted page glyphs by kind, folder glyphs, a
  link badge, a red broken-link badge and a dimmed icon for an unreadable folder. None are
  borrowed assets.
- **Breadcrumb.** The current folder is the bold last crumb. When the path does not fit, the
  breadcrumb collapses from the left into "…" and keeps the root. Back and forward have a
  visible disabled state.
- **Status bar.**
  - Left: item count · hidden count.
  - Right: selected count · selected size (bold count).
  - "Loading…" appears while a listing is busy.
- **Empty states.** P2-3 is done. Each state has one quiet line and a keycap hint for the next
  action:
  - empty folder: `Alt`+`↑` to go to the parent folder
  - hidden items only: `Ctrl`+`H`
  - unreadable folder: the reason plus `Alt`+`↑`
- **Error banner.** It has a tinted danger surface, a hairline border and an icon. The title and
  the reason sit at two text weights, and the banner has a close button. A successful navigation
  clears it.
- **Keyboard.** Every chord in `docs/keymap.md` goes through the registry: activation, back,
  forward, up, refresh, select all, clear selection, toggle hidden, and cursor moves with and
  without `Extend`. Mouse side buttons map to back and forward.
- **HiDPI.** At 2x and 1.5x, glyphs are rasterised at physical size. Hairlines stay 1px, and the
  marker and focus ring snap to pixels.
- **Turkish.** All strings fit at 1x, including "Değiştirilme", "Kabuk betiği", "JPEG
  görüntüsü" and "Kırık bağlantı". ğ, ş, ı, İ and ç shape correctly.
- **Immediacy.** The previous listing stays on screen while the next one loads, so there is no
  blank flash. Column widths are fixed, so rows do not shift. The loop still idles on events.

## BLOCKING

None.

## OPTIONAL (Phase 2 polish, in priority order)

1. **O1. Density.** Rows are 28dp, the header is 30dp and the toolbar is 52dp. That is GNOME
   comfortable, not File Pilot's documented dense, keyboard-first feel. The Phase 0 review
   asked for a compact row of about 22–24dp plus a comfortable option. Add a `compact` density
   with these values:
   - row 24dp
   - header 26dp
   - toolbar 44dp
   - status bar 26dp

   Drive it from the `metrics` namespace and expose it as a command. Consider making compact
   the default. This is the single change that most improves the premium feel.
2. **O2. Keyboard dismissal of the banner.** The notice can only be closed with the mouse. Make
   `Escape` dismiss the notice when the selection is already empty, or add a `notice.dismiss`
   command. Give it a keymap row.
3. **O3. Duplicate error text.** When the current folder itself is unreadable, the banner and
   the empty state both say "You don't have permission to access it." Keep the empty state,
   which has a next action, and suppress the banner for `OpenFolderFailed` on the folder being
   shown. Keep the banner for failures on other targets.
4. **O4. Unit alignment in Size.** "980 bytes" and "7 bytes" sit beside "88.1 kB", so the
   numbers do not line up. Either right-align the number in a fixed slot and left-align the
   unit after it, or use "B". Translate the unit in Turkish as well.
5. **O5. Toolbar glyph family.** Back and forward are thin chevrons, but Up is a stemmed arrow
   that looks heavier. Use one family: three chevrons, or three arrows with the same stroke.
6. **O6. Typographic apostrophe.** "Couldn't" uses a straight `'` next to curly quotes. Use `’`
   in the English catalog.
7. **O7. Date year.** Dates in the current year could drop the year ("Nov 10 10:00") to quiet
   the column. This is optional; keep the full form in a tooltip later.
8. **O8. Folder size cell.** The Size cell is blank for folders. `docs/features.md` lists
   sorting by count, so show a muted item count ("12 items"), filled lazily so the UI never
   blocks.
9. **O9. Zebra rows in dark.** The alternating rows are almost invisible in dark at about 1.03:1.
   That is acceptable, but `docs/features.md` says alternating rows are on by default, so lift
   the dark stripe slightly or make the stripe a setting.
10. **O10. Type-ahead.** The Phase 0 review suggested type-ahead (P2-4). It is not in the Phase 2
    roadmap or in `features.md`. If it is added, it is a Navigator extra: give it a `CommandId`
    and use locale-aware Turkish i/İ matching.

## Notes for Phase 3 (tabs and split panes)

- **Focus.** Two panes need one obvious active pane. The inactive pane should use the existing
  `selection_inactive` grey with no cursor ring, and the active pane should get a 1–2px accent
  top edge or header underline. Never mark the active pane by colour alone.
- **Tab strip.** Make the strip 32–34dp in compact density and keep it on the same 4dp grid.
  Put the tab label in `text_secondary`; the active tab gets `text_primary`, a raised surface
  and no heavy borders. Truncate long folder names in the middle. Turkish labels must fit, so
  test with "Çalışma Alanı" and longer names.
- **`Ctrl`+click conflict.** This is still open (see `docs/keymap.md`). Decide on middle-click,
  `Ctrl`+double-click or both before any tab UI lands.
- **Density per pane.** Panes halve the available width. Before split panes ship, check that the
  column set degrades in this order: Type hides first, then Modified shrinks to the date only.
- **Restore.** Preserve the scroll position and cursor when switching tabs.
  `docs/features.md` lists "scroll position preserved" for Phase 2 navigation, and tabs should
  match it.

## Notes for Phase 4 (left sidebar: Places, Storage, Bookmarks)

The owner explicitly asked for a left sidebar.

- **Layout.** Use a resizable column that defaults to about 220dp, with a minimum of 160dp and a
  collapse toggle bound to a command. It sits left of the list panel, on the window
  `background`, not on a raised card, so the list stays the primary surface. The splitter is a
  1px hairline with an 8dp hit area.
- **Section headers.** Places, Storage and Bookmarks use 11–12dp SemiBold in `text_muted`, in
  sentence case. Turkish: "Yerler", "Depolama", "Yer imleri". Headers can be collapsed, and the
  collapse state persists.
- **Rows.** Rows use the list's row height and the same selection language: tint plus leading
  marker for the folder currently shown, and hover tint only on hover. Icons use the same 16dp
  family as the list.
- **Storage rows.** Show a thin 3dp usage bar under the name. Use the accent colour, and
  `danger` above 90%. Draw the free-space figure with tabular figures. `docs/features.md` lists
  volume usage with a bar (phases 2 and 4).
- **Eject.** Use a trailing icon that is visible on hover and on keyboard focus. Mount and
  unmount errors use the existing banner.
- **Keyboard.** The sidebar needs a focus command, then Up/Down/Enter inside it, `Escape` to
  return to the list, and a visible focus ring. Add each new action to the keymap and the
  palette.
- **Breadcrumb.** When the sidebar lands, the breadcrumb gets dropdown previews and item counts
  (Phase 4 in `features.md`). Keep the current collapse-to-"…" behaviour, and make "…" open a
  menu of the hidden segments. Show "Home" as the root for paths under `$HOME` (already
  translated as "Ev").
- **Address bar.** Wire up `FocusAddressBar`, which is registered but unbound. Typing or pasting
  a path must not disturb the list layout.
