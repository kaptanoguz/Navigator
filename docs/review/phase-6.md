# Phase 6 design review

Reviewer: design-reviewer. Branch `phase-6` at `11f5c30`.

Evidence:

- **The 10 Phase 6 captures (`p6-*`).** Photo in fit and 1:1, video in dark/light and en/tr,
  text in en/tr, a folder, list thumbnails at 2x, and bottom placement in a narrow window.
- **13 extra renders of the current tree.** I built them myself (RelWithDebInfo, xvfb, temp
  HOME, config, cache and thumbnail dirs) with fixtures I generated:
  - binary file
  - unreadable file (mode 000)
  - damaged JPEG
  - fake and real PDF
  - 16x16 transparent PNG at 2x
  - 3000x600 banner
  - 1920x1080 MKV with stereo audio
  - 1080x1920 MP4
  - empty folder
  - three-item mixed selection
  - video at the bottom in Turkish
  - an image-switch run with `dump`
- **Code and docs.** I read `inspector_view.cpp` (states, toolbar rule, tooltips), the Phase 6
  i18n entries and `providers.cpp` (placeholder routing). I checked the intent against
  "Inspector (preview)", "Thumbnails, icons, cache" and "Navigator extras" in
  `docs/features.md` and the Phase 6 section of `docs/keymap.md`.

No capture shows a File Pilot name, logo or artwork. Video poster, video metadata and "Play" are
documented as Navigator extras, as the Phase 5 notes asked.

Note: `p6-text-light-1x-en.png` is stale. It wraps code in the middle of words ("Inspe|ctor").
The current tree wraps at word boundaries (re-rendered), so that capture is not a finding.

## Verdict

**Ship Phase 6 after B1 and B2.** The owner asked for premium photo and video previews, and this
phase delivers them:

- **Images.** EXIF orientation is applied before layout. Metadata is grouped (File, Image,
  Camera, Location, Video, Audio) and empty groups are skipped. Fit is the default, and the
  floating zoom pill with the minimap reads as a real image viewer, not a debug panel.
- **Video.** The poster with its translucent play disc looks good on a busy frame (test bars) and
  on a black frame (the ring carries it).
- **Bottom placement.** In a narrow window it puts the preview and the metadata side by side, so
  the strip stays useful instead of becoming a letterbox.
- **States.** Loading, unsupported, binary, permission denied, damaged and empty-folder states
  all exist, and they share one composition (glyph, title, muted detail, then the File group).

Two items block. A thumbnailer-backed PDF preview shows the thumbnail's pixel size as the
document's "Image > Dimensions". The Inspector's icon buttons have no tooltips, so "Play" does
not say that it hands the file to another app.

## What works

- **Layout and density.** The header has the name, a muted "kind · size" subtitle, and the lock
  and placement buttons at the right. Group headings and key/value rows use the status-bar type
  scale, keys are muted and values primary, and the key column sizes to the longest key. Rhythm
  is even at 1x and 2x.
- **Image stage.**
  - A neutral checkerboard shows only behind transparent pixels (Logo.png, light), not across the
    whole stage.
  - The zoom toolbar shows on hover, while the Inspector has focus, and whenever the image is not
    in fit, so the way back is always visible. The Phase 5 notes asked for exactly that.
  - The minimap has a framed viewport rectangle and sits clear of the toolbar.
- **Video metadata.** It reads like a spec sheet: duration, resolution, frame rate, codecs,
  container, bit rate, channels and sample rate. A 1080x1920 clip gets a portrait poster with the
  right aspect.
- **Text view.**
  - Monospace with a separate line-number gutter.
  - Wrapped continuation lines carry no number, which is right.
  - Breaks fall at word boundaries.
  - It is virtualized.
- **Folder view.** A count line ("3 klasör · 7 dosya"), zebra rows, sizes right-aligned, folders
  first. It looks like a miniature of the list, as intended.
- **List thumbnails.** The row height is unchanged and thumbnails sit on the icon grid with the
  1px frame. The type icon stays until the thumbnail lands (broken.jpg keeps its glyph). They are
  crisp at 2x.
- **Contrast (sampled from renders).**

  | Text | Dark | Light |
  |---|---|---|
  | Metadata keys | 6.8:1 | 7.0:1 |
  | Group headings | 4.8:1 | 5.2:1 |
  | State detail text | 6.8:1 | 7.0:1 |
  | Line numbers | ~4.4:1 | 4.9:1 |
  | Play button label | 8.0:1 | |

## BLOCKING

**B1. Thumbnailer previews of documents present false image metadata.**

- **What I saw.** A real PDF shows its page through the system thumbnailer, which is acceptable
  as a Navigator extra. But the Inspector then lists "Görüntü / Boyutlar 768 × 1024" (Image /
  Dimensions). That is the thumbnail's pixel size, not anything about the document.
- **Scope.** The same path will mislabel any other mime a system `.thumbnailer` covers: office
  documents, fonts, ebooks.
- **Docs mismatch.** `docs/features.md` says PDF preview is not claimed in Phase 6. The
  `inspector.coming_soon_pdf` state therefore shows only when no thumbnailer is installed, so
  behaviour depends on the machine.
- **Fix.**
  1. When an image comes from a thumbnailer for a non-image mime, do not emit the Image group, or
     label it "Preview" without dimensions.
  2. Decide one documented behaviour: keep the page preview and list it in the Navigator extras
     table as Phase 6, or route PDF to "Preview coming soon".
  3. Make the list thumbnail match the choice. Today the list shows the PDF type icon while the
     Inspector shows the page.

**B2. Inspector buttons have no tooltips, so "Play" does not say it opens another app.**

- **What I found.** `InspectorView::tooltip_` is declared and cleared but never set. Lock,
  placement, fit, 1:1, grid and minimap are icon-only. "Play" / "Oynat" is an accent button on a
  poster with a play disc, which every user will read as in-app playback. Clicking it instead
  launches an external player, which is a surprise.
- **Fix.** Wire tooltips through the same path the title bar uses (`BrowserView::draw_tooltip`):
  - Play: "Open in <default app name>" / "<uygulama> ile aç". Fall back to "Open in the default
    player" / "Varsayılan oynatıcıda aç" when GIO gives no app name.
  - Lock: "Lock to this item" / "Bu öğeye kilitle".
  - Placement: "Move (auto, right, bottom)" / "Taşı (otomatik, sağ, alt)".
  - Fit, 1:1, grid and minimap: the palette strings.

  Also add a small "opens externally" arrow glyph after the "Play" label, so it reads as a
  hand-off even before hovering. Keep the play disc; it is the right affordance on the poster.

## OPTIONAL

1. **Header and File group repeat each other.** The subtitle says "JPEG image · 719 kB", and the
   File group repeats Kind and Size. Make the subtitle the most useful one-liner per kind:
   - image: "4000 × 3000 · 719 kB"
   - video: "0:08 · 1280 × 720 · 3.2 MB"
   - text: "312 lines · 7.8 kB"

   Keep the full rows below. The multi-selection view says "3 items selected" three times
   (header, body, Items row); show a per-type breakdown instead ("1 image, 1 video, 1 text").
2. **Keyboard path for zoom.** Fit, 1:1, grid and minimap are mouse-only until the palette lands
   in Phase 8. While the Inspector has focus (after a click, the same scoping as the sidebar),
   route `+` / `-` / `0` (fit) / `1` (1:1) to the inspector commands and say so in
   `docs/keymap.md`. They never fire while the list has focus. This is Navigator's choice, not
   attributed to File Pilot.
3. **Tiny images.** A 16x16 icon in fit at 2x is an 8 dp speck in a 600 dp stage. My Phase 5
   note said "no upscaling past 1:1 in fit"; I am revising it:
   - Images of 64 px or less get an integer nearest-neighbour upscale to about 128 dp.
   - The stage height follows the image aspect, capped, instead of staying at a fixed height.
4. **Extreme aspect ratios in list thumbnails.** wide-banner.jpg (5:1) becomes a 2-3 px blue line.
   Above about 3:1, centre-crop to the thumbnail box, or keep the type glyph with a small colour
   swatch.
5. **Video "Kind" is just "Video".** The Kind row says "Video", while images say "JPEG image".
   Use "MP4 video" / "Matroska video" (TR "MP4 videosu"). Also, Container shows "QuickTime / MOV"
   for an `.mp4`. Map the mov/mp4 demuxer family to "MP4" when the extension or brand says so.
6. **Permissions as "664".** Show "rw-rw-r-- (664)" and the owner. The phase-7 permissions panel
   can expand on this.
7. **Camera / Camera.** The Camera group's first row is also "Camera" ("Fotoğraf makinesi" twice
   in TR). Rename the row to "Model".
8. **Text view metadata.** The text view takes the whole card and has no footer. Add a quiet
   one-line footer: lines · encoding · line endings, and "Showing the first N" when truncated.
9. **Empty folder.** "0 folders · 0 files" above "This folder is empty" is redundant, and the
   glyph is a document outline. Hide the count line when it is zero and use the folder glyph.
10. **Error tone.** Permission denied uses the same red "!" as a damaged file. Permission is a
    state, not a fault: use a lock glyph in the muted colour and keep red for damaged or failed.
11. **Default width.** At 1400 px the right card is about 355 dp, so a 4:3 photo renders at about
    320 x 240. For a premium photo feel, default to about 30% of the window (min/max clamps
    unchanged). It remembers its width per placement.
12. **Line numbers in dark.** The gutter text measures about 4.4:1, just under AA for small text.
    Raise one step (to the `text_muted` used for keys) or darken the gutter fill.
13. **Folder preview thumbnails.** `features.md` lists "folder image prefetch for the Inspector"
    for this phase. Show 16 dp thumbnails in folder-preview rows for images, using the list's
    cache.
14. **Responsiveness check on hardware.** My `dump` run reported `inspector_max_frame_ms 78.8`
    while switching between three images at 2x. That was under xvfb with a software renderer,
    which inflates it, so I am not blocking on it. QA should repeat it on a real GPU. If any frame
    exceeds 16 ms there, it becomes a blocker under the performance bar in `CLAUDE.md`.

Not verified: the pixel grid at high zoom (the commands cannot reach that zoom from a script),
the loading skeleton and crossfade, and live refresh when a file changes.

## Turkish wording

- **"Denetçi" should become "Önizleme".** "Denetçi" reads as "auditor / supervisor" (a person who
  inspects compliance). A Turkish user would not guess that it is a side panel showing a file.
  The common reference points are:
  - Windows Explorer TR: "Önizleme bölmesi" and "Ayrıntılar bölmesi"
  - macOS Finder TR: "Önizleme" / "Bilgi"

  This panel is mostly preview with metadata under it, so "Önizleme" is the natural word.
  "Ayrıntılar" alone would undersell the image and video stage. It is also consistent with the
  state strings, which already say "Önizleme yok" and "Önizleme yükleniyor". Suggested strings:

  | Key | Turkish |
  |---|---|
  | `inspector.title` | Önizleme |
  | `cmd.view.inspect` | Önizleme panelini göster/gizle |
  | `cmd.inspector.toggle_lock` | Önizlemeyi bu öğeye kilitle |
  | `cmd.inspector.zoom_fit` | Görüntüyü panele sığdır |
  | `cmd.inspector.cycle_placement` | Önizleme panelini taşı (otomatik, sağ, alt) |
  | `inspector.unlocked_gone` | Kilitli öğe artık yok; önizleme yeniden imleci izliyor |

  The English "Inspector" stays.
- **"Görüntü genel bakışı" should become "Mini harita".** It is clumsy; "Mini harita" is the term
  Turkish users know from code editors. Use "Mini haritayı göster/gizle".
- **"Oynat" can stay**, with the B2 tooltip.

Everything else reads naturally: "Kare hızı", "Örnekleme hızı", "Kapsayıcı", "Saat yönünde 90°
döndürülmüş", "İkili dosya", "Okuma izniniz yok".

## Notes for Phase 7 (file operations, trash, undo, drag and drop)

- **Progress.** Copy and move progress should reuse the Phase 5 flatten pattern: a thin bar under
  the title bar, a stop button in the status bar and no modal. Show bytes and items, the current
  file name middle-elided, and a rate/ETA only after about 2 s of stable samples.
- **Conflicts.** Use one sheet in the Inspector card's surface style, not a system dialog:
  - It shows both files side by side, with the Phase 6 thumbnail, size and modified date. This is
    where the Inspector work pays off again.
  - Actions: Replace, Skip, Keep both, plus "Apply to all".
  - Keyboard: `Enter` is the safe default (Keep both), `Escape` skips.
- **Trash.** Trash is the default delete and never asks for confirmation. Show a notice in the
  existing notice style with "Undo" ("Geri al"). Permanent delete (`Shift+Delete`) always
  confirms, names the count, and says it cannot be undone ("geri alınamaz").
- **Undo.** Built-in undo/redo is a Navigator extra (`features.md`). It must not be attributed to
  File Pilot.
  - Undo works per operation, with a palette-visible history label: "Undo: move 3 items to
    Photos".
  - It never undoes across a volume that has since been unmounted; say why instead of failing
    silently.
- **Drag and drop.**
  - Reuse the tab-split drop target styling for folder rows, the sidebar and breadcrumbs.
  - Show a drag badge with a count and the first item's thumbnail.
  - Modifiers follow GNOME conventions (`Ctrl` copy, `Shift` move, `Alt` ask) and the cursor
    shows the resulting action.
  - Spring-loaded folders take about 700 ms and are off with `--no-animations`.
  - On Wayland, test drag between Navigator and Nautilus/Files in both directions.
- **Inspector during operations.** If the shown file is moved, renamed or trashed, the Inspector
  follows the new path, or drops to "The file no longer exists". It should not flash an error.
  The lock should survive a rename.
- **Every new action** gets a `CommandId`, a palette string in English and Turkish, and a row in
  `docs/keymap.md` before implementation.
