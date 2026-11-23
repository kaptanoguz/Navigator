# Phase 5 design review

Reviewer: design-reviewer. Branch `phase-5` at `6df4535`. The UI is unchanged in `721f861`.

Evidence: the 21 Phase 5 captures (`p5-*`: filter, selection, no match, three panes, flatten
progress, flatten with a query, whole-volume index and scan, types popup and real input; dark
and light; 1x and 2x; English and Turkish). I also made three extra renders under xvfb, in the
sandboxed fixture with `--no-index` and a temporary config dir. They show a plain-substring flatten query, a types
popup with every type hidden (Turkish, light), and type-to-filter with no match (Turkish, dark). I read
`browser_view.cpp` (column plan, Folder column, inline parent), `browser_search_view.cpp`
(field, progress, status), `types_popup.cpp`, the Phase 5 theme tokens and the Phase 5 i18n
entries. I checked the intent against "Search and filter" in `docs/features.md` and the Phase 5
section of `docs/keymap.md`. No capture shows a File Pilot name, logo or artwork.

## Verdict

**Ship Phase 5 after fixing B1.** This is the strongest phase so far. The filter field sits
where the Phase 4 notes asked, reads as a sibling of the address bar, and its count chip,
clear button, match underline and no-match empty state are consistent and quiet. Flatten
streams results without a modal, shows a thin bar under the title bar and a stop button in
the status bar, and adds a Folder column that collapses to an inline muted parent in
narrow panes. The match colour passes AA on every row state in both themes. One item blocks:
whole-drive search shows nothing that marks it as a drive-wide search once a query is
typed, and the breadcrumbs still name the starting folder.

## What works

- **Placement and craft.** The field sits right of the breadcrumbs and left of the flatten,
  types and star buttons. It uses the address bar's radius and focus ring. When the filter is
  applied but not focused, the field keeps a softer accent border, so a filtered pane still
  looks filtered after `Tab`. Placeholders change with the mode ("Filter" / "Search
  subfolders" / "Search drive", with a muted `Ctrl+F` hint), which is a good touch.
- **Focus (Phase 4 B1 rule).** While the field has focus the list cursor draws as inactive.
  When the list has focus it draws the full ring (`p5-filter-selected-*`). There is never more
  than one focus ring.
- **Match highlight.** `match` is accent ink plus an underline at 55% alpha. The row
  background never changes, so colour is not the only cue. Contrast, computed from the palette
  with the stripe, hover and selection layers composited:

  | | plain | stripe | hover | selected+hover | inactive sel.+hover |
  |---|---|---|---|---|---|
  | Dark | 8.9 | 8.2 | 6.8 | **4.57** | 5.2 |
  | Dark, dimmed pane | 9.7 | 9.0 | 7.6 | 5.1 | 5.8 |
  | Light | 7.6 | 7.3 | 6.5 | 5.4 | 5.5 |
  | Light, dimmed pane | 6.6 | 6.3 | 5.6 | 4.7 | 4.8 |

  All pass 4.5:1. The tightest case is dark, selected and hovered at the same time, so there is
  no room to lighten the selection fill without retuning `match`. A match hidden inside an
  elided name tints the ellipsis, which is a good detail.
- **Count chip.** "11 of 55" / "11 / 55" uses the translucent accent chip (text 8.2:1 dark,
  6.0:1 light). It turns to `danger` at zero, and the status bar repeats the count. In a 3-pane
  layout the chip shortens to "11" instead of pushing the clear button out.
- **Empty states.** "No items match “zebra”" with an `Escape` keycap and "Clear filter".
  "Every type here is hidden" with a `Ctrl Shift F` keycap and "Choose types". Both use the
  Phase 2 empty-state style. The Turkish strings ("“zebra” ile eşleşen öğe yok", "Buradaki
  tüm türler gizli") read naturally and fit.
- **Flatten.** The button shows a toggled state. The 2dp bar is indeterminate under the title
  bar, never modal. The status bar shows a spinner, the running count, "N folders read",
  "Searching…" and a Stop pill. The Folder column is relative to the root
  ("Documents/Reports 2026") and middle-elided. Below 260dp of name room it becomes a muted
  parent after the name. Ranked results drop the sort chevron until a header is clicked, so
  the header never claims an order the list does not have (checked in `rv-flatq-plain`).
- **Types popup.** It anchors under the funnel button, which carries a count badge ("1",
  "12"). It has a tri-state "All types" row and per-type rows with icon, extension, muted MIME
  label and right-aligned counts. Unchecked rows dim. A header link shows "7 items hidden",
  and the status bar adds "1 type hidden". Keyboard: the `list.cursor_*` commands move the
  highlight, `list.activate` toggles, and `Escape` / `Ctrl+Shift+F` close. `Escape` in the list
  never clears the types filter, because that filter is a saved view setting. That is the right
  call.
- **Turkish, HiDPI, light/dark.** At 1366px nothing clips in Turkish ("55 öğede 11 sonuç ·
  1 gizli", "240 öğe · 4 klasör tarandı · Aranıyor… · Durdur"). The 2x captures are crisp:
  the chip, keycaps, checkboxes and the 1.6dp checkmark all land on whole pixels. The light
  theme is not a washed-out copy of the dark one: `match` and `chip_text` darken to #2C49B8.
- **Feels immediate.** Filtering is per keystroke, flatten rows arrive in batches while the list
  stays scrollable, and type-to-filter goes straight into the field with no extra step.

## BLOCKING

**B1. A whole-drive search does not say it is drive-wide once you type.** After
`Ctrl+Shift+E`, the only scope cue is the placeholder "Search drive", and it disappears
with the first character. After that:
- The breadcrumbs still show "Home › Documents".
- The flatten button has the same toggled look as a subtree flatten.
- The field looks exactly like a `Ctrl+E` search.

So `p5-volume-index-*` and `p5-flatten-query-*` look the same apart from the Folder column
(absolute vs. relative paths). `Ctrl+E` and `Ctrl+Shift+E` differ by one modifier. Users will
read "133 matches" as "133 in Documents". Phase 7 file operations will act on these results.
Fix (any one is enough; the first is preferred):
- Show a persistent scope token at the start of the field while flattened: "Subfolders" /
  "Alt klasörler" or "Drive" / "Sürücü", plus the volume name if it fits. Style it as a small
  `chip` with `chip_text`, so it survives typing and is the first thing the eye lands on.
- Or, in volume mode, switch the breadcrumbs to the volume root (volume icon + "File
  System") and give the flatten button a distinct drive variant.

## OPTIONAL

1. **Unchecked checkbox contrast.** `keycap_border` on the popup surface is 1.8:1 (dark) and
   1.9:1 (light). WCAG 1.4.11 asks for 3:1 for a control's state. The dimmed row label
   backs up the state, so this is not blocking. Use `text_muted`, or a dedicated
   `control_border` of at least 3:1. This is the one place in Phase 5 that looks a little
   unfinished at 1x.
2. **Index wording on an empty drive query.** "Index ready (60,728 items) · 15 folders read ·
   Searching…" mixes two engines: the index is ready, yet a walk is running. Say what is
   happening. Use "Listing drive…" / "Sürücü listeleniyor…" for the walk. Show "Index ready" only
   when a query is being answered from the index. Or invite a query: "Type to search 60,728
   indexed items".
3. **Absolute paths in the drive Folder column.** "/home/ada/Documents/…" spends about 10
   characters per row on the home prefix. Show `~/` for paths under home (keep the absolute
   path in the tooltip and in Copy path).
4. **Root rows in a flatten say "Documents".** Rows directly in the root repeat the root's
   name. Leave them empty, or use a muted "·", so that subfolder paths stand out. The inline-parent
   mode already does this.
5. **Narrow-pane breadcrumbs.** With the filter focused in a 3-pane layout, the current folder
   elides to "Docum…" while the ancestors stay ("Home › Docum…" in the uneven layout). Drop the
   ancestors first ("… › Documents"), and elide the current folder only after that. The current
   folder name is the most useful crumb.
6. **"≈" toggle discoverability.** The fuzzy toggle is a bare glyph. Fuzzy is on by default in
   flatten and off in the folder filter, which is intentional and sensible. Give the button a
   tooltip that shows the mode and command ("Fuzzy matching · on"), and make the off state a bit
   more visible than `text_muted` on the field.
7. **Two kinds of "hidden" in one status bar.** "48 of 55 · 1 type hidden · 1 hidden" puts two
   meanings of "hidden" side by side. Say "1 hidden file" for dotfiles ("1 gizli dosya").
8. **Turkish placeholder.** "Süz" is correct but abrupt as a field hint. "Süzgeç" or "Bu
   klasörde süz" reads more like a label. The command name "Bu klasörü süz" is fine.
9. **Types popup solo action.** A modifier-click or context action like "Only this type" /
   "Yalnızca bu tür" would make "show me just PDFs" one gesture instead of two clicks.

## Notes for Phase 6 (Inspector and thumbnails)

The owner asked explicitly for photo and video previews. `docs/features.md` covers the
Inspector, image fit / 1:1, EXIF orientation, grid and minimap. It also states "video playback
not claimed". So the video poster frame and the "Play" hand-off are Navigator's own choices.
Document them that way, not as File Pilot behaviour.

- **Placement and focus.** The Inspector opens with `Space` (type-to-filter already leaves a
  leading `Space` free). Place it on the right by default, with bottom placement as an option.
  It follows the list cursor, so arrow keys keep moving the list while the Inspector updates.
  It never takes focus unless clicked. Show it on the `surface_raised` card with the same 1dp
  `border` as popups. "Lock" is a toggled icon button in its header, styled like the flatten
  toggle.
- **Image inspector.**
  - Fit by default; 1:1 means one image pixel per *device* pixel at 2x.
  - Apply EXIF orientation before any layout.
  - Use a neutral checkerboard for transparency in both themes.
  - Zoom keys must not steal list keys while the list has focus. Put fit / 1:1 on Inspector
    header buttons and in the palette.
  - Decode off-thread and cancellably, downscaled to the view size, with a memory cap. Huge
    PNGs or TIFFs must never stall a frame.
  - Show a skeleton of the right aspect ratio at once, then crossfade in the image (none with
    `--no-animations`).
- **EXIF and metadata.**
  - Show a two-column key/value list in the status-bar typography: muted keys, primary values,
    tabular numerals.
  - Group it: File (size, type, dates), Image (dimensions, colour space), Camera (make/model,
    lens, exposure, ISO, focal length, taken at).
  - Show GPS only as coordinates, with no map fetch and no network.
  - Make values copyable.
  - Skip empty groups instead of listing "—" rows.
- **Thumbnails in list and grid.**
  - Use the freedesktop Thumbnail Managing Standard: `$XDG_CACHE_HOME/thumbnails/{normal,
    large,x-large,xx-large}`, MD5 of the file URI, and `Thumb::URI` / `Thumb::MTime`
    validation. Write failures to `fail/navigator-<ver>/`.
  - Pick the size bucket from the *device* pixel size (2x list icons need `large`).
  - Generate missing thumbnails through the system thumbnailers (`/usr/share/thumbnailers/
    *.thumbnailer`) in a bounded, cancellable worker pool. Prioritise visible rows and drop
    off-screen requests.
  - Keep the type icon until the thumbnail lands. Never change the row height. Keep the 1px
    `icon_page_border` frame so thumbnails sit on the same grid as the glyphs.
  - In headless and test runs, write thumbnails only under `--cache-dir`, never into the real
    cache, by the same rule as the name index. Never thumbnail files inside the thumbnail
    directory itself.
- **Video.**
  - Show a poster frame from the thumbnail cache or system thumbnailer, with a centred
    translucent play disc.
  - Show metadata (duration, resolution, codec, frame rate, audio channels) in the same
    key/value list.
  - "Play" (button, and `Enter` on the poster) opens the default application through the
    platform layer (GIO / `xdg-open`), exactly like `list.activate`. It needs a stable
    `CommandId` and a palette entry.
  - No in-app playback.
  - If no thumbnailer is installed, show the type icon with "No preview available" /
    "Önizleme yok" and the metadata, not an error.
- **States to design up front:** loading, unsupported type, unreadable file, file changed while
  shown (refresh in place), very small images (no upscaling past 1:1 in fit mode unless
  asked), and the multi-selection summary (count and total size).
- **Grid view** (if it lands in Phase 6): reuse the selection fill, cursor ring, match underline
  (under the caption) and inline-rename metrics from the list, so the two views read as one
  product. Captions should be two lines at most, middle-elided, with the extension always
  visible.
