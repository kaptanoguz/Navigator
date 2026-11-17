# Phase 4 design review

Reviewer: design-reviewer. Branch `phase-4` at `0cae4f8`.

Evidence: the Phase 4 captures (sidebar, GoTo, address bar, context menu, bookmark rename,
icon-only mode and three panes; dark and light; 1x and 2x; English and Turkish) and five extra
renders under xvfb with a temporary HOME and `--config-dir`: `F6` into the sidebar, a focused
section header with a hovered network volume, a 560px-tall window, and an empty Turkish config
(no bookmarks, no recents). Note that `p4-sidebar-light-1x-tr.png` predates the last build: it
still shows the old "yok" badge, while the current catalog says "kayıp". I read
`src/ui/sidebar/sidebar_view.cpp`, `src/ui/path/path_entry.cpp`, the theme palette, the
Phase 4 i18n entries and the sidebar focus wiring in `src/ui/shell/app.cpp`. I checked the
intent against "Sidebar, bookmarks, recents" and "GoTo and address bar" in `docs/features.md`
and the Phase 4 section of `docs/keymap.md`. There is no File Pilot name, logo or artwork
anywhere.

## Verdict

**Ship Phase 4 after fixing B1.** The sidebar is the best-crafted surface in Navigator so far.
It uses the same row and selection language as the list, so it reads as one product. Section
headers are quiet. Usage bars, badges and the icon-only mode are done with care, and the
sidebar holds up with three panes at 1366px. GoTo and the address bar are fast and clear.
One item blocks: when the sidebar has keyboard focus, the active pane still draws its cursor
ring at full strength, so the window shows two focus rings and you cannot tell where the
arrow keys will go.

## What works

- **Density and rhythm.** Rows are 28dp, on the same grid as the list. Icons and labels are
  inset the same in every section. Section headers are 11dp `text_muted` with the chevron
  right-aligned, so badges, percents and chevrons all end on one right edge. The window
  `background` surface keeps the sidebar quieter than the list.
- **Containing place.** Clicking a place highlights it with the list's selection fill and the
  accent marker bar. The highlight moves with the active pane: in the three-pane capture,
  Downloads follows the active top-right pane. Places shown in other panes get no highlight.
  This is exactly what the Phase 3 note asked for.
- **Usage bars.**
  - Each bar sits under its label, with the percent right-aligned on the shared edge.
  - The bar uses the accent colour and switches to `danger` at 90% (the NAS at 92%).
  - Hovering shows free space ("680 GB free" / "680 GB boş").
  - In icon-only mode, a tiny version of the bar sits under each drive icon.
- **Badges.** The offline badge is a neutral pill outlined in `keycap_border`. The missing badge
  has red text and a red outline. Offline rows also get a crossed-out icon and dimmed text. Text
  contrast meets AA in both themes:
  - Dark theme: secondary text 8.0:1, muted 5.6:1, danger 8.4:1 on `background`.
  - Light theme: secondary 6.5:1, muted 4.8:1, danger 5.2:1.
- **Icons.** One stroke family at one weight. Each XDG place has its own glyph, and drives,
  USB sticks and network shares are easy to tell apart.
- **Keyboard path.** `F6` lands on the containing place. The sidebar gets the B1 accent
  underline under its top bar plus a row focus ring. Section headers can take focus and show
  "Clear" for Recents when focused. The context menu greys out "Move up" on the first bookmark.
  Inline rename uses the standard text field.
- **Empty states.** "Henüz yer imi yok" comes with a `Ctrl` `D` keycap hint, and the empty
  Recents state explains itself. Both fit at 220dp in Turkish.
- **GoTo.**
  - The input row comes first, then sections (Folders / Recents / Places & bookmarks), each
    row with its path in muted text.
  - A keycap legend at the bottom matches the Phase 3 popups.
  - The offline bookmark keeps its badge.
  - Errors use the `danger_surface` banner.
- **Address bar.** `Ctrl+L` turns the breadcrumbs into an accent-ringed field. The completion
  dropdown sits directly under it. The star and lock stay put while editing.
- **HiDPI.** At 2x, strokes, the 1.5dp focus rings, bars and keycaps stay crisp and aligned to
  the pixel grid.
- **Turkish.** "Son kullanılanlar", "Dosya Sistemi", "çevrimdışı" and "kayıp" all fit. Long
  bookmark names are truncated in the middle to make room for the badge ("Backup…otos").
- **1366px, three panes.** With the 220dp sidebar, each pane is about 560px wide, and O5 column
  degradation removes Type in the narrow left pane. The sidebar switches to icon-only below
  1000dp, which is derived every frame and never persisted, as recommended.

## BLOCKING

1. **B1. Two focus rings when the sidebar has focus.** After `F6`, the sidebar row has its ring
   and the top-bar underline, but the list cursor on "Invoices 2026" keeps its full-strength
   ring too (`p4-r-f6.png`). The cause is in `app.cpp`: the workspace view gets
   `.focused = focused` (the window's focus), not "the pane has keyboard focus". When the
   sidebar has focus, pass a "keyboard focus is elsewhere" state into the workspace. The
   active pane keeps its active marker but draws its cursor the way an inactive pane does (no
   ring, as in the Phase 3 three-pane captures). The address bar and GoTo field should apply
   the same rule. This is the same problem as Phase 3 B1: the keyboard is the main way to
   drive Navigator, so the user has to see where keystrokes go.

## OPTIONAL (in priority order)

1. **O1. GoTo: Enter on a partial path.** For "~/Do" or "~/B", the highlight starts on the
   literal "Go to ~/Do" row. Enter on that row resolves a path that does not exist and shows an
   error, even when the only match is "Belgeler". Suggestion: when the typed text does not
   exist and there are completions, move the default highlight to the first completion, so
   Enter goes there and the "Go to" row is the preview. Keep the literal row first when the
   typed path does exist.
2. **O2. GoTo error is shown twice.** The banner and the right side of the typed row both say
   "Bilinmeyen değişken $YOK". The banner also joins two messages with a double space
   ("“…” yok  Bilinmeyen değişken $YOK"). Show only the root cause (the unknown variable) in
   the banner, and keep the row clean or give it the red icon only.
3. **O3. The sidebar gives no sign that it scrolls.** At 560px tall, the whole Storage section
   is below the fold, and nothing shows there is more (`p4-r-short.png`). There is a top
   shadow once you have scrolled. Add the matching bottom fade, and show a thin overlay
   scrollbar on hover, like the list.
4. **O4. Icon-only mode loses state.** The "missing" bookmark looks like a normal folder, and
   four bookmarks share one folder glyph. Add a small danger dot for missing and a muted dot for
   offline on the icon. Tooltips already carry the label and the badge text.
5. **O5. Hover makes the volume row jump.** On hover, the percent is replaced by "680 GB free",
   which is wider, so the label is truncated again ("nas.local:/…t/media" becomes
   "nas.loc…edia"). Keep the percent and move free space into a tooltip, or show it as a muted
   second line under the bar.
6. **O6. Network volume label.** "nas.local:/…t/media" is the raw mount source. Prefer
   "media on nas.local" / "nas.local üzerinde media", which is the convention GNOME users
   know, and keep the full source in the tooltip.
7. **O7. File System is split from Storage.** The root volume sits at the bottom of Places with
   a usage bar, while the other volumes are under Storage. Show it as the first row of Storage
   so all usage bars are in one group, or keep it in Places without a bar.
8. **O8. Recents repeat Places.** With the cap at 7, Recents fills up with Downloads, Documents
   and navigator, which are already Places or Bookmarks (`p4-three-*`). Leave folders that
   already appear in Places or Bookmarks out of Recents, or show them last.
9. **O9. Usage bar has only two colours.** It goes straight from accent to danger at 90%. A
   `warning` colour from 80% makes the bar a useful early signal. Check that it meets 3:1
   against the track in both themes.
10. **O10. GoTo icon vs sidebar icon.** Bookmarks have a star in GoTo but a folder in the
    sidebar. Pick one. A folder icon in both places is clearer, because the star already means
    "bookmark this" in the title bar.
11. **O11. The lock is a dead control.** `view.toggle_view_lock` is a no-op stub, but the lock
    icon shows in every pane's title bar. Hide it until it works, or at least give it a
    "coming later" tooltip with no hover fill.
12. **O12. Address-bar dropdown width.** The dropdown is as wide as the whole field (about
    1000px) for one-word folder names. Anchor it at the caret's segment and cap it at about
    360dp, as GoTo already does.
13. **O13. Eject.** The Phase 2 notes asked for an eject button on removable and network rows
    on hover and focus. Phase 4 doesn't have one. Add it once `platform` exposes unmount.
14. **O14. Turkish copy.** The hint "{key} bu klasörü ekler" drops what the folder is added to.
    "{key} ile yer imlerine ekle" is clearer and still fits. "Git:" (with a colon) next to the
    English "Go to" (without one) is fine, but make the punctuation match.

## Notes for Phase 5 (search and filter UI, `Ctrl+E` flatten)

- **Where it lives.** Make the folder filter a field in the active pane's title bar, to the
  right of the breadcrumbs and to the left of the star and lock. It is per pane, not global.
  Each pane in a three-pane layout keeps its own filter. Use the address bar's field styling
  (accent ring when focused) so the two read as siblings.
- **Focus.** Apply the B1 rule here too: while the filter field has focus, the list cursor
  draws as inactive. `docs/features.md` says most commands still work while the filter has
  focus (Phase 8 row), so the field must take only its editing keys, as the Phase 4 focus
  scoping already does.
- **Filter state.** A filtered list needs a persistent sign that it is filtered. Use an accent
  chip in the title bar ("12 of 340" / "340 öğeden 12'si") with a clear (×) button, and give
  the status bar the same count. When there are no matches, show an empty state in the list
  body, using the Phase 2 empty-state style with a "Clear filter" keycap hint, not a blank
  pane.
- **Flatten (`Ctrl+E`).** It is a view mode, so show it as a toggled icon-button state next to
  the filter, plus a relative-path column (or a muted parent path under each name) so rows
  with the same name can be told apart. Listing must stay streamed and cancellable. Show
  progress as a thin indeterminate bar under the title bar, not a modal, with the running count
  in the status bar. `Escape` cancels the walk first and then leaves flatten.
- **Match highlight.** Use the accent colour at a moderate weight on matched characters, and do
  not change the row background, because the selection fill already uses it. Check AA on the
  selection fill in both themes.
- **Sidebar.** Clicking a sidebar place while a filter is active should clear the filter (it
  is navigation). Flatten should not add every walked subfolder to Recents.
- **Width.** At 1366px with three panes, a pane's title bar is about 560px. The filter field
  needs a minimum width of about 140dp and should collapse to an icon button below that, before
  it squeezes the breadcrumbs.
