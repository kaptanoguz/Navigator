# Design review: Phase 7 (file operations, trash, undo, drag and drop)

Reviewer: design-reviewer. Scope: density, typography, contrast, keyboard paths, empty states,
immediacy, wording EN/TR, and above all whether destructive actions are hard to trigger by
accident and outcomes are unambiguous. Compared with the intent in `docs/features.md`,
`docs/keymap.md` (Phase 7) and `docs/dnd.md`. No File Pilot assets or branding were involved.

Method: scratch build of the current tree (Release), run only under Xvfb with a temporary HOME,
temp config/cache/thumbnail/trash dirs and generated fixture folders. Rendered: delete confirm
(EN light, TR dark), conflict sheet (EN light, TR dark and light, after the footer fix), progress
panel with four queued operations (EN dark, TR light), Trash view empty (EN dark, TR light) and
with items (EN dark, TR light), empty-Trash confirm (TR dark), undo notice (EN light), inline
rename (TR dark), rename with a name clash (EN light), new folder (EN dark), drag ghost over a
folder row, over a file row, over itself, onto the sidebar Home row and onto the Trash row.
The untrashable sheet and the quit-with-running-ops sheet could not be driven by the debug
script; they were reviewed from `src/ui/ops/file_ops.cpp`, `ops_view.cpp` and `i18n.cpp` only.

## Verdict

**Ship Phase 7 after one trivial fix.** The safety design is sound: the default delete is
undoable and silent, permanent delete and empty-Trash always confirm, the confirm sheets open
with focus on Cancel and need `Tab` then `Enter` (or a click) for the destructive button,
auto-repeat never re-fires file commands, the conflict default (`Enter`) is the non-destructive
"Keep both", Replace states where the old file goes, drops never delete, and outgoing drags only
offer a copy. I found nothing that can lose data by accident. Visually the surfaces are coherent,
calm and legible in both themes and both languages.

## BLOCKING

1. **Raw i18n key shown on the progress chip.** With two or more operations running (or queued)
   the status-bar chip reads `ops.chip.many.other` in English and Turkish. The catalog has
   `ops.chip.many` but `tr_count` looks up `.one` / `.other` (`ops_view.cpp:215`,
   `i18n.cpp:768`). This is the primary progress affordance and the first thing a user sees
   when they start a second copy, so it is a visible defect, not a data risk. Fix: add
   `ops.chip.many.one` (unreachable) and `ops.chip.many.other` ("{n} operations" / "{n} işlem").
   I scanned every `tr_count` base in `src/ui`; this is the only one without an `.other` entry.
   A catalog test that asserts every `tr_count` base has `.other` would keep it from returning.

Nothing else is blocking.

## OPTIONAL (ordered by value)

Safety and clarity

2. **Turkish says "Çöp" and "Çöp kutusu" for the same thing.** The sidebar row, tab title and
   toolbar use "Çöp" ("Çöpü boşalt", "Çöp boşaltılsın mı?", "Çöpü aç"), while notices, commands
   and the drag ghost use "çöp kutusu" ("2 öğe çöp kutusuna taşındı", "Çöp kutusuna taşı"), and
   the conflict button says "eskisi çöpe gider". Pick one. "Çöp kutusu" is the GNOME/Nautilus TR
   term and is unambiguous; use it in full wherever space allows and keep "Çöp" only for the
   narrow sidebar label. "Çöp kutusuna taşı" and "Kalıcı olarak sil" are themselves clear and
   cannot be confused: one moves, one erases, and the confirm body says "Bu işlem geri alınamaz.
   Öğeler çöp kutusuna gitmez." which closes the gap. The English has the same small drift:
   "Move to Trash" (command) vs "Moved 2 items to the trash" (notice). Use "Trash" in both.
3. **Conflict sheet footer drops its most important hint first.** Hints that do not fit are left
   out from the end, and `Esc` is last, so in Turkish and in English light 1x the footer shows
   Enter, Tab, Space, F2 but not "Esc, cancel operation". The overflow is fixed (nothing is drawn
   past the card), but the user loses how to bail out. Put `Esc` first, or shorten the labels
   ("Seçili düğme", "Sonraki", "Hepsine", "Ad", "İptal").
4. **Replace gives no hint when it would overwrite a newer file with an older one.** The
   Older/Newer chips are good; make the case obvious by tinting the Replace button's chip or
   adding "(older)" to the confirmation line. The label itself ("Replace (old goes to Trash)" /
   "Değiştir (eskisi çöpe gider)") is excellent and should stay. Same for "Replace (permanent)"
   if a danger tint is not already applied.
5. **Undo notice lifetime and discoverability.** It lasts 8 s (12 s for errors) and the "Undo"
   link is small accent-coloured text, which is acceptable, but: it does not pause while the
   pointer is over it, and it does not say `Ctrl+Z`. Pause the timer on hover and add the
   chord as a muted suffix ("Undo  Ctrl+Z"). `Ctrl+Z` keeps working after the notice is gone,
   which is the real safety net; the Operations panel "Recent" list should show an Undo entry
   too (not verified here).
6. **Partial failure drops the Undo link.** If 3 of 5 items were trashed, the notice offers
   "Details" and no Undo, although `Ctrl+Z` still reverses the 3 (the undo record is kept).
   Offer Undo for the part that succeeded, worded "Undo 3".
7. **No dimming behind the sheets.** The delete confirm, conflict and empty-Trash sheets float
   over a fully lit list. They are modal for the keyboard, so a light scrim (about 20 %) would
   say so and make the dangerous sheet stand out. The 1 px card edge and shadow are enough in
   dark, weak in light.
8. **Delete confirm shows names but not where.** For `Shift+Delete` in a flattened search or
   multi-folder selection, add the folder (muted) next to the first names. Item and byte totals
   are already there and are good. Empty-Trash confirm shows only a top-level count; add the
   size ("3 items, 680 bytes") because one folder can hold thousands of files.
9. **Same-folder drop is styled as an error.** Dragging a file over its own folder shows a red
   outline on the whole list and "Cannot drop here". It is a no-op, not a failure; use a
   neutral outline and "Already here" / "Zaten burada". Red should stay for real refusals
   (read-only, into itself).

Keyboard

10. **No keyboard path to cancel or pause a running operation** (`ops.cancel`, `ops.toggle_pause`
    and `ops.toggle_panel` are unbound; the panel never takes focus, by design). Mouse-only for
    a long copy is a gap. Phase 8 (palette, keymap editor) closes it; until then consider
    binding `ops.toggle_panel` (it is harmless) and make `Esc` in the list, when no filter or
    popup is active and an operation is running and the panel is open, focus the panel's stop
    button instead of cancelling directly.
11. **Trash view toolbar has Restore and Empty Trash but no "Delete permanently".** It exists
    on `Delete` and the context menu with a confirmation, which is safe; add it to the
    context menu hint or toolbar overflow for discoverability.

Visual polish

12. **Inline editor selection is hard to see in dark.** The stem selection fill is barely
    lighter than the field, and the caret sits on the "." so the dot looks missing ("İş planı|txt").
    Raise the selection fill to the accent at about 35 % and keep the caret at the selection
    end. The red clash bubble in light ("“todo.txt” already exists in this folder") is clear
    and well placed.
13. **Operations panel density.** Rows are 86 px tall with a lot of air for queued items; with
    four operations the panel already covers half of the second pane. A queued row can drop its
    bar and use one line ("Waiting for another operation"). Destinations use only the folder
    name ("to B"); give the full path in a tooltip so two folders called "B" are not confusable.
    Rate and ETA ("253 kB/s · 36 s left") and the middle-elided current file are well judged.
14. **Drag ghost.** Legible in both themes (about 12 px type, 1 px border, shadow) and the
    second line ("Move here · into Photos", "Move to Trash") is the right information. The
    secondary line is low contrast in dark (about 3.5:1); lift it one step. The ghost covers
    sidebar rows right under the pointer; a larger offset to the lower right would keep the
    hovered target visible. Spring-loading works and the pane outline reads as a drop target.
15. **Conflict sheet stays up while a stale "Copied “x”" toast shows behind it** when the user
    started with `Ctrl+C`. That toast is the clipboard confirmation, correct but momentarily
    confusing next to a sheet about copying; clear clipboard notices when a sheet opens.
16. **Immediacy.** Sheets, notices and the Trash view appear within the first frame in all
    runs; "Counting items…" and "Waiting for your decision" states are honest. No jank seen in
    the progress panel while 240 files copied (150 ms per-entry delay).

Wording checks, EN/TR, all fine unless listed above: "Delete 2 items permanently?" / "2 öğe
kalıcı olarak silinsin mi?", "Vazgeç" on confirm vs "İptal" elsewhere (acceptable, "Vazgeç" is the
better word for a confirm), "Gelen / Burada olan", "Daha eski / Daha yeni", "Kararınız bekleniyor",
"Çöp boş" with a helpful hint line, "Geri al: 2 öğeyi çöp kutusuna taşı" (imperative after a
colon is understandable; "Geri al" alone is the button). Trash empty state is clear and calm.
Default focus and button order are consistent: Cancel left and focused in confirms; "Keep both"
rightmost and primary in the conflict sheet, "Cancel operation" isolated on the left.
Quit sheet: default focus index 0 is "Keep working" (safe).

Note on the supplied screenshot `p7-confirm-light-2x-en.png`: the focus ring is on the red
button there; that came from a scripted `Tab` before the capture. A fresh run
(`rv7-confirm-en.png`) starts on Cancel, as specified.

## Notes for Phase 8

- **Command palette.** The labelled-undo strings already exist (`cmd.edit.undo_labeled`);
  show them live ("Undo: move 3 items to Photos"). Palette needs `ops.cancel` / `ops.toggle_pause`
  with an operation picker when several run, and "Show Operations". It also fixes finding 10.
  Hide commands that are no-ops in the current focus (Trash view, sidebar), rather than showing
  them greyed.
- **Searchable context menu.** Rows now gain Cut, Copy, Paste into folder, Rename, Move to
  Trash, Delete permanently. Keep "Delete permanently" visually separated and last, in the
  danger colour, with no accelerator letter, so typing a prefix never selects it by accident.
  Search must never auto-activate a single result on Enter when that result is destructive.
- **Keymap editor.** Warn when a user binds a bare key (no modifier) to `file.delete_permanently`
  or `trash.empty`, and refuse to bind them to a key sequence prefix that type-to-filter also
  uses. Keep the sheets' keys (Tab, Enter, Esc, Space, F2) outside the remappable set.
- **Remappable sequences and aliases.** Destructive commands should never run from an alias
  without the confirmation sheet; aliases may point to `file.trash`, not around the sheet.
  Mind the Turkish layout (AltGr punctuation) already noted in Phase 3.
- **Notices.** Move to a notice centre once the palette exists, so Undo is reachable after the
  8 s toast without remembering `Ctrl+Z`.
