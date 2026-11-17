# Phase 3 design review

Reviewer: design-reviewer. Branch `phase-3` at `0a120b3`.

Evidence: the Phase 3 captures (three panes in dark and light, 1x and 2x, English and Turkish;
drag preview; Search Tabs; save prompt; layout picker; tab overflow) and an xdotool run against a
real window on a private Xvfb display (new tabs, drag-to-split, splitter hover and drag,
middle-click close, Search Tabs with no match, tab context menu). I sampled pixel colours in the
three-pane captures to measure how strong the inactive-pane dimming is. I read
`src/ui/workspace/{workspace_view,popups,tab_strip}.cpp`, the theme diff (`dimmed()`,
`tab_bar_height`, the new type styles) and the Phase 3 sections of `docs/keymap.md`. I checked
the intent against "Panes, tabs, layouts" in `docs/features.md`.

## Verdict

**Ship Phase 3 after fixing B1.** Tabs, splits, drag-to-split and the popups are well made, and
they read as the same product as Phase 2. There is no File Pilot name, logo or artwork. One
item blocks: in a multi-pane layout you cannot tell at a glance which pane is active, and in
the light theme you can hardly tell at all. Every keyboard command goes to the active pane, so
this is a functional problem as well as a visual one.

## What works

- **Tab strip.**
  - The strip is 34dp in compact density, on the 4dp grid, as the Phase 2 note asked.
  - Inactive tabs are flat `text_secondary`. The active tab has a raised surface, `text_primary`
    and Medium weight, with no heavy borders.
  - Hairline separators appear only between two inactive, unhovered tabs.
  - The close button shows on the active tab and on hover. It gets its own hover fill.
  - Long names are truncated in the middle ("Quarterly plan…drafts 2026").
- **Overflow.** Tabs shrink to a minimum width. Past that, the strip scrolls, the clipped edges
  fade instead of ending in a hard cut, and a chevron opens a list of all tabs. The active tab
  is kept in view.
- **Drop affordances.**
  - The edge preview is a rounded accent tint with an 80% accent outline over exactly the half
    the new pane will take.
  - A filled accent pill names the result ("Split right" / "Sağa böl"), and its text uses
    `on_accent`.
  - The tab being dragged dims to 35% in its strip, and a floating ghost follows the pointer.
  - Inside a strip, the insertion marker is an accent bar with round caps.
  - `Escape` cancels the drag.
- **Splitters.** The splitter is invisible at rest. On hover it shows a 55% accent line, which
  rises to 90% while you drag. A double-click makes the sizes equal.
- **Popups.**
  - Search Tabs, the layout picker and the overflow list share one design: a search field,
    a small section header with a count, rows with a leading marker on the highlighted row,
    and a footer of keycaps (`↑` `↓` Move, `Enter`, `Esc`).
  - Paths are truncated in the middle.
  - The no-match state is one quiet line ("Eşleşen sekme yok").
  - Deleting from the picker needs a second press and turns the row danger-tinted, so a stray
    click cannot delete a layout.
  - The save prompt sits on a scrim. It has a focused accent field, a helper line, and
    Cancel / Save buttons; Save is the primary button.
- **Tab context menu.** It shows the chord on the right of each item. "Close left tabs" is
  disabled on the first tab, and keyboard navigation skips disabled items.
- **Turkish and HiDPI.** "Yerleşimi kaydet", "Panelleri, sekmelerini ve boyutlarını kaydeder.",
  "Kapatılan sekmeyi yeniden aç  Ctrl+Shift+T" and "Sağa böl" all fit at 1x and 2x. At 2x the
  hairlines stay 1px, and the accent edge and insertion bar snap to pixels.
- **Immediacy.** Dimming fades over 140ms and respects `--no-animations`. Drag feedback is the
  same frame as the pointer.

## BLOCKING

**B1. The active pane is not obvious, especially in the light theme.**

Measured on the three-pane captures:

| Token | Dark: active → inactive | Light: active → inactive |
|---|---|---|
| list surface | `#171A21` → `#14171D` | `#FFFFFF` → `#FBFBFC` (about 1.03:1) |
| primary text | `#E8EAF0` → `#D7DAE2` | `#17191E` → `#31353E` |

The tab strip, toolbar and breadcrumb are not dimmed at all, so they look identical in every
pane. The only marker of the active pane is a 2dp accent edge on top of its active tab, about
80px wide. The cursor ring also helps, but only when the pane has a cursor.

`dimmed()` mixes the surface 45% toward `background`. In light that gap is only
`#FFFFFF` vs `#F5F6F8`, so the dimming does nothing. Phase 2 asked for this explicitly: "Never
mark the active pane by colour alone" and add "a 1–2px accent top edge or header underline".
The fix needs both parts:

1. **Add a pane-level marker that is not a tint.** Do one of these:
   - draw a 1px accent outline (about 60% alpha) on the active pane's list frame, or
   - draw a 2dp accent underline across the full width of the active pane's tab strip.

   Show it only when there is more than one pane.
2. **Make the dimming strong enough to read in light.** Use per-theme amounts instead of one
   mix. In light, move the inactive surface toward `background` fully, or slightly below it.
   Also dim the inactive pane's chrome: breadcrumb text to `text_secondary`, and the current
   crumb from SemiBold to Medium.

   Keep the existing AA assertion in `theme_test.cpp`. Add a test that the active and inactive
   surfaces differ by at least a minimum ΔL in both themes.

Acceptance: in `p3-three-light-1x-en`, someone who has not seen the app before points to the
active pane within one second, and the same is true with the selection cleared.

## OPTIONAL (in priority order)

1. **O1. Search Tabs "Pane 3" badge.** Panes have no visible numbers anywhere else, so "Pane 3"
   means nothing to the user. Replace it with a tiny layout glyph that highlights where the
   pane sits (the save prompt's icon already draws a layout), or use "Left", "Top right" and
   so on.
2. **O2. Duplicate tab names.** Six tabs all called "p3fx" are hard to tell apart. When names
   collide in a strip, add the shortest distinguishing parent ("Projects › src" vs
   "Website › src"), as editors do. Show the full path as a tooltip on hover.
3. **O3. Keyboard path to delete a layout.** `layout.delete` is unbound, and the picker's delete
   button only responds to the mouse. Bind `Delete` on the highlighted row in the picker, using
   the same two-step confirmation, and add the hint to the footer.
4. **O4. Keyboard path to the tab context menu.** "Close left tabs" can only be reached with the
   mouse. Open the menu for the active tab with `Menu` / `Shift+F10`, and list both in
   `docs/keymap.md`. The Phase 8 palette covers this too, but the menu key is the native
   convention.
5. **O5. Narrow-pane columns.** At about 430px, Modified is dropped while Size stays. Size is
   blank for folders, so a folder-only pane shows nothing but names (`p3-int-f`). Follow the
   order Phase 2 asked for: hide Type first, then shorten Modified (time only for today, date
   only otherwise), then hide Size, and hide Modified last. At about 660px Type is already
   hidden while the Name column has spare room. Check that the Type threshold is not too eager.
6. **O6. Save prompt footer.** It is the only popup without the keycap footer. Add
   `Enter` Save · `Esc` Cancel for consistency, or put the keycap inside the buttons.
7. **O7. Overflow fade start.** At the left fade, a separator can sit alone in the faded area
   (`p3-ov-crop`). Skip the separator next to a clipped tab.
8. **O8. Drag ghost at the window edge.** The ghost is clipped by the window's right edge
   (`p3-int-b`). Clamp it inside the window, or flip it to the left of the pointer near the
   edge.
9. **O9. Theme toggle position.** The sun/moon button lives in the tab strip of whichever pane
   is top-right, so it moves when the layout changes. Give it a fixed window-level home; Phase
   4's sidebar footer or a title area is a natural place.
10. **O10. Drop preview label.** Consider adding the target folder to the pill
    ("Split right · Music") when you drop on another pane. This is cheap and helps in layouts
    with three or more panes.

## Notes for Phase 4 (left sidebar: Places, Storage, Bookmarks, Recents)

The owner explicitly wants this. The Phase 2 notes still apply: 220dp default, 160dp minimum, a
collapse command, the window `background` surface, 11–12dp section headers, the row and
selection language, a usage bar on storage rows, and an eject button on hover and focus. In a
multi-pane world, the following also apply:

- **One sidebar per window, not one per pane.** It sits left of the whole pane tree and is not
  a node in the layout. Saved layouts and the session store its width and collapsed state
  separately from the pane tree. Loading a layout must not move it.
- **It acts on the active pane.** Clicking a place navigates the active pane. `Ctrl`+click and
  middle-click open it in a new tab in the active pane, which matches the list. Dragging a
  place onto a pane or a pane edge reuses the Phase 3 drop affordances and opens the place
  there, or splits.
- **Reflect the active pane.** Highlight the place that contains the active pane's folder using
  the selection marker. The highlight moves with `F6`. Places shown in other panes get no
  highlight (or a muted dot at most). This also helps with B1.
- **Recents.** Show them below Bookmarks, capped at about 8, with a "Clear" action under its
  own `CommandId`. Recents come from Navigator's own navigation history. Do not claim
  this is a File Pilot behaviour unless `docs/features.md` lists it.
- **Focus order.** `F6` / `Shift+F6` should cycle sidebar → panes in reading order → sidebar.
  The sidebar gets the same visible focus treatment as the B1 active-pane marker. `Escape`
  returns to the last active pane.
- **Width budget.** With the sidebar open, a three-pane layout on a 1366px screen leaves panes
  of about 380px. O5 (column degradation) must be done before Phase 4 ships. Consider
  collapsing the sidebar to icons only (about 48dp) automatically below a window width
  threshold, and keep the user's explicit collapse state separate from that.
- **Bookmarks from tabs.** Dragging a tab onto the Bookmarks section bookmarks its folder. Show
  the same insertion marker as the tab strip.
- **Strings.** Turkish: "Yerler", "Depolama", "Yer imleri", "Son kullanılanlar". Check that
  "Son kullanılanlar" fits at 160dp.
