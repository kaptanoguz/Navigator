#!/bin/sh
# Phase 7 headless app, adversarial end to end (owner: qa). The real binary on the SDL dummy
# driver, steps through --debug-script (key:CHORD goes through handle_key -> sheets, text fields,
# keymap -> App::Impl::file_command, exactly like a keystroke). EVERY run uses a temporary HOME,
# XDG_DATA_HOME, XDG_CONFIG_HOME, XDG_CACHE_HOME and explicit --config-dir, --cache-dir and
# (unless the case is "no trash") --trash-dir inside the fixture. Nothing touches the owner's
# home, the real XDG trash or a real volume. After every run the temporary HOME and
# XDG_DATA_HOME are asserted to hold no trash and no files at all.
#
#   1. copy + cut/paste between two panes, F7 / Shift+F7, then Undo: a move comes back (the
#      Phase 7 acceptance), the undo of a copy goes to the trash (content kept), redo repeats it.
#   2. Delete -> Trash view -> Enter / trash.restore restores (acceptance); Delete in the Trash
#      view and trash.empty always confirm and cancel by default.
#   3. Conflict sheet: Enter = Keep both, Escape cancels the operation and touches nothing,
#      Replace goes through the trash, apply-to-all.
#   4. Permanent delete is refused without the confirmation: Shift+Delete then Escape / Enter (focus
#      starts on Cancel) / confirm:no / the run ending with the sheet open all keep the file; only
#      Tab + Enter (or confirm:yes) removes it, and then it is not in the trash.
#   5. A headless run without --trash-dir never trashes (the file stays) and never creates a trash
#      under the temporary HOME or XDG_DATA_HOME; Delete then asks for the permanent delete and
#      only confirm:yes removes the file.
#   6. Quit while an operation runs opens the sheet; the default keeps working (the copy finishes
#      whole); cancel-and-quit leaves no .part file and no truncated file.
#   7. Focus scoping: with the address bar open, the filter field focused, the inline rename editor
#      open or the sidebar focused, Delete / Shift+Delete / F2 / Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z /
#      Ctrl+X / Ctrl+C / Ctrl+V / Ctrl+Shift+N / F7 never act on files.
# Usage: ops_app_smoke.sh <navigator binary>
# Exit 0 = pass, 77 = skipped (no usable dummy driver), anything else = failure.
set -u
bin=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-ops-app-XXXXXX") || exit 1
case $root in /tmp/*|"${TMPDIR:-/tmp}"/*) ;; *) echo "refusing a fixture outside the temp dir: $root"; exit 1 ;; esac
trap 'chmod -R u+rwx "$root" 2>/dev/null; rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export HOME="$root/home" XDG_CONFIG_HOME="$root/xdg-config" XDG_CACHE_HOME="$root/xdg-cache" \
    XDG_DATA_HOME="$root/xdg-data"
unset NAVIGATOR_DEBUG_VOLUMES NAVIGATOR_DEBUG_OPS_DELAY_MS DISPLAY WAYLAND_DISPLAY
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
fails=0
xfails=0
n=0
name=init

fail() { echo "FAIL [$name]: $1"; [ -f "$root/run.log" ] && tail -15 "$root/run.log" | sed 's/^/    /'; fails=$((fails + 1)); }
# Open bugs are reported as XFAIL (never fail the run) unless NAVIGATOR_QA_STRICT=1.
xfail() { # bug id, message
    if [ "${NAVIGATOR_QA_STRICT:-0}" = 1 ]; then fail "$1: $2"; else echo "XFAIL [$name] $1: $2"; xfails=$((xfails + 1)); fi
}
want() { [ -e "$1" ] || [ -L "$1" ] || fail "missing $1"; }
gone() { [ ! -e "$1" ] && [ ! -L "$1" ] || fail "still there: $1"; }
same() { [ "$(cat "$1" 2>/dev/null)" = "$2" ] || fail "content of $1 is '$(cat "$1" 2>/dev/null)', want '$2'"; }

# Fresh fixture: A (left pane) and B (right pane) and an empty fixture trash.
fresh() {
    rm -rf "$root/A" "$root/B" "$root/trash" "$root/cfg" "$root/cache"
    mkdir -p "$root/A/sub" "$root/B" "$root/trash" "$root/cfg" "$root/cache"
    for f in alpha beta gamma delta; do printf 'A %s\n' "$f" >"$root/A/$f.txt"; done
    printf 'notes\n' >"$root/A/notes.md"
    printf 'turkish\n' >"$root/A/Çalışma ğ.txt"
    printf 'inner\n' >"$root/A/sub/inner.txt"
    printf 'B alpha (other version)\n' >"$root/B/alpha.txt"
    printf 'B beta (other version)\n' >"$root/B/beta.txt"
    A=$root/A
    B=$root/B
    T=$root/trash
}

# run <script> [--no-trash]: one app run; exit code recorded in $rc, output in run.log.
run() {
    script=$1
    trashflag="--trash-dir $T"
    [ "${2:-}" = "--no-trash" ] && trashflag=""
    n=$((n + 1))
    # shellcheck disable=SC2086
    "$bin" --frames "${FRAMES:-30}" --no-session --no-index --no-thumbnails --no-animations --lang en \
        --config-dir "$root/cfg" --cache-dir "$root/cache" $trashflag \
        --debug-script "$script" "$A" >"$root/run.log" 2>&1
    rc=$?
    [ $rc -eq 0 ] || fail "app exited with $rc"
    check_home
}

# Nothing under the temporary HOME / XDG_DATA_HOME, no trash anywhere but --trash-dir.
check_home() {
    [ -z "$(find "$HOME" "$XDG_DATA_HOME" -mindepth 1 2>/dev/null | head -1)" ] ||
        fail "the app wrote under the temporary HOME / XDG_DATA_HOME: $(find "$HOME" "$XDG_DATA_HOME" -mindepth 1 | head -3 | tr '\n' ' ')"
    [ -z "$(find "$root" -iname 'Trash' -not -path "$root/trash" 2>/dev/null | head -1)" ] ||
        fail "a trash directory exists outside --trash-dir"
    [ -z "$(find "$root" -name '.*.navigator-*.part' 2>/dev/null | head -1)" ] ||
        fail "leftover .part temp files"
}
trash_count() { ls -A "$T/files" 2>/dev/null | wc -l; }

if ! SDL_VIDEODRIVER=dummy "$bin" --frames 2 --no-session --no-index --no-thumbnails --config-dir "$root" \
    --cache-dir "$root" "$root" >/dev/null 2>&1; then
    echo "SKIP: the dummy video driver is not usable here"
    exit 77
fi

# ---------------------------------------------------------------------------------------------
name=pane-ops; fresh
pre="cmd:pane.split_right;go:$B;focus:0"
# copy gamma and cut/paste delta to the other pane (neither name exists there)
s="$pre;click:gamma.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitops"
s="$s;focus:0;click:delta.txt;cmd:edit.cut;focus:1;cmd:edit.paste;waitops"
run "$s"
want "$B/gamma.txt"; want "$A/gamma.txt"; want "$B/delta.txt"; gone "$A/delta.txt"
same "$B/alpha.txt" "B alpha (other version)"; same "$B/beta.txt" "B beta (other version)"
echo "OK: copy and cut/paste into the other pane"

fresh
# the acceptance: a MOVE between two panes comes back with Undo
s="$pre;click:notes.md;cmd:edit.cut;focus:1;cmd:edit.paste;waitops"
run "$s"; want "$B/notes.md"; gone "$A/notes.md"
fresh
s="$pre;click:notes.md;cmd:edit.cut;focus:1;cmd:edit.paste;waitops;cmd:edit.undo;waitops"
run "$s"; want "$A/notes.md"; gone "$B/notes.md"; same "$A/notes.md" notes
[ "$(trash_count)" -eq 0 ] || fail "undo of a move used the trash"
echo "OK: a move between two panes comes back with Undo (acceptance)"

fresh
# undo of a copy goes to the trash (never an unlink); redo copies again
s="$pre;click:gamma.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitops;cmd:edit.undo;waitops;dump:$root/d2.txt"
run "$s"
want "$A/gamma.txt"; gone "$B/gamma.txt"; want "$T/files/gamma.txt"; same "$T/files/gamma.txt" "A gamma"
want "$T/info/gamma.txt.trashinfo"
fresh
s="$pre;click:gamma.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitops;cmd:edit.undo;waitops;key:Ctrl+Shift+Z;waitops"
run "$s"; same "$B/gamma.txt" "A gamma"; want "$T/files/gamma.txt"
fresh
s="$pre;click:gamma.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitops;cmd:edit.undo;waitops;key:Ctrl+Y;waitops"
run "$s"; same "$B/gamma.txt" "A gamma"
echo "OK: undo of a copy trashes it (content kept); Ctrl+Shift+Z and Ctrl+Y redo"

fresh
# F7 copies, Shift+F7 moves to the other pane's folder; both undo
s="$pre;click:delta.txt;key:F7;waitops;click:notes.md;key:Shift+F7;waitops"
run "$s"; want "$B/delta.txt"; want "$A/delta.txt"; want "$B/notes.md"; gone "$A/notes.md"
fresh
s="$pre;click:delta.txt;key:F7;waitops;click:notes.md;key:Shift+F7;waitops;key:Ctrl+Z;waitops;key:Ctrl+Z;waitops"
run "$s"; gone "$B/delta.txt"; want "$A/delta.txt"; want "$A/notes.md"; gone "$B/notes.md"
fresh
# one pane only: F7 is a no-op with a notice
run "click:delta.txt;key:F7;key:Shift+F7;waitops"
want "$A/delta.txt"; [ "$(ls "$B" | wc -l)" -eq 2 ] || fail "F7 with one pane changed B"
echo "OK: F7 and Shift+F7 (and their undo); a single pane is a no-op"

fresh
# Turkish names through the clipboard round trip
s="$pre;click:Çalışma ğ.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitops"
run "$s"; same "$B/Çalışma ğ.txt" turkish
echo "OK: Turkish file names survive copy and paste"

# ---------------------------------------------------------------------------------------------
name=trash-restore; fresh
s="click:notes.md;key:Delete;waitops;cmd:trash.open;waitops;trashclick:notes.md;key:Enter;waitops"
run "$s"
want "$A/notes.md"; same "$A/notes.md" notes; [ "$(trash_count)" -eq 0 ] || fail "trash not empty after restore: $(ls "$T/files")"
fresh
s="click:notes.md;key:Delete;waitops;cmd:trash.open;waitops;trashclick:notes.md;cmd:trash.restore;waitops"
run "$s"; want "$A/notes.md"
fresh
# Delete trashes (never deletes); Undo restores it; the Trash view lists Turkish names
s="click:Çalışma ğ.txt;key:Delete;waitops;dump:$root/d3.txt"
run "$s"; gone "$A/Çalışma ğ.txt"; want "$T/files/Çalışma ğ.txt"
grep -q 'Path=.*%C3%87al%C4%B1%C5%9Fma%20%C4%9F.txt' "$T/info/Çalışma ğ.txt.trashinfo" ||
    fail "the .trashinfo Path is not GIO-escaped: $(cat "$T/info/Çalışma ğ.txt.trashinfo" 2>/dev/null)"
fresh
s="click:alpha.txt;key:Delete;waitops;key:Ctrl+Z;waitops"
run "$s"; want "$A/alpha.txt"; [ "$(trash_count)" -eq 0 ] || fail "undo of a trash left an item in the trash"
fresh
# a folder with content is trashed whole and restored whole
s="click:sub;key:Delete;waitops;cmd:trash.open;waitops;trashclick:sub;cmd:trash.restore;waitops"
run "$s"; same "$A/sub/inner.txt" inner
echo "OK: Delete -> Trash view -> restore (acceptance), Enter restores, undo of a trash"

fresh
# In the Trash view Delete is a permanent delete that confirms, focus on Cancel; trash.empty too.
s="click:notes.md;key:Delete;waitops;click:gamma.txt;key:Delete;waitops;cmd:trash.open;waitops"
s="$s;trashclick:notes.md;key:Delete;confirm:no;waitops;dump:$root/d4.txt"
run "$s"; want "$T/files/notes.md"
fresh
s="click:notes.md;key:Delete;waitops;click:gamma.txt;key:Delete;waitops;cmd:trash.open;waitops"
s="$s;trashclick:notes.md;key:Delete;key:Enter;waitops;cmd:trash.empty;key:Escape;waitops"
run "$s"; want "$T/files/notes.md"; want "$T/files/gamma.txt"
fresh
s="click:notes.md;key:Delete;waitops;click:gamma.txt;key:Delete;waitops;cmd:trash.open;waitops"
s="$s;trashclick:notes.md;key:Delete;confirm:yes;waitops"
run "$s"; gone "$T/files/notes.md"; gone "$T/info/notes.md.trashinfo"; want "$T/files/gamma.txt"
gone "$A/notes.md"
fresh
s="click:notes.md;key:Delete;waitops;click:gamma.txt;key:Delete;waitops;cmd:trash.open;waitops"
s="$s;cmd:trash.empty;confirm:yes;waitops"
run "$s"; [ "$(trash_count)" -eq 0 ] || fail "trash.empty confirmed but items remain: $(ls "$T/files")"
[ "$(ls -A "$T/info" 2>/dev/null | wc -l)" -eq 0 ] || fail "info files left after emptying"
echo "OK: permanent delete in the Trash view and trash.empty always confirm (Cancel by default)"

# ---------------------------------------------------------------------------------------------
name=conflict; fresh
pre="cmd:pane.split_right;go:$B;focus:0"
# Enter = Keep both
s="$pre;click:alpha.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitconflict;key:Enter;waitops"
run "$s"; same "$B/alpha.txt" "B alpha (other version)"; same "$B/alpha (2).txt" "A alpha"
fresh
# Escape cancels the operation: nothing is touched
s="$pre;click:alpha.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitconflict;key:Escape;waitops"
run "$s"; same "$B/alpha.txt" "B alpha (other version)"; gone "$B/alpha (2).txt"; [ "$(trash_count)" -eq 0 ] || fail "Escape trashed something"
[ "$(ls "$B" | wc -l)" -eq 2 ] || fail "Escape on the conflict sheet changed B: $(ls "$B")"
fresh
# Escape on the second conflict of a multi-item paste: the first (non-conflicting) item that
# completed stays, nothing else is touched, no .part file
printf 'A new\n' >"$A/0-new.txt"
s="$pre;click:0-new.txt;click:alpha.txt:ctrl;click:beta.txt:ctrl;cmd:edit.copy;focus:1;cmd:edit.paste;waitconflict;key:Escape;waitops"
run "$s"; same "$B/alpha.txt" "B alpha (other version)"; same "$B/beta.txt" "B beta (other version)"
gone "$B/alpha (2).txt"; gone "$B/beta (2).txt"; want "$B/0-new.txt"
fresh
# Replace goes through the trash; the old version is recoverable
s="$pre;click:alpha.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitconflict;conflict:replace;waitops"
run "$s"; same "$B/alpha.txt" "A alpha"; same "$T/files/alpha.txt" "B alpha (other version)"
fresh
s="$pre;click:alpha.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitconflict;conflict:replace;waitops;cmd:edit.undo;waitops"
run "$s"; same "$B/alpha.txt" "B alpha (other version)"
fresh
# Skip, apply to all
s="$pre;click:alpha.txt;click:beta.txt:ctrl;cmd:edit.copy;focus:1;cmd:edit.paste;waitconflict;conflict:skip:all;waitops"
run "$s"; same "$B/alpha.txt" "B alpha (other version)"; same "$B/beta.txt" "B beta (other version)"
fresh
# A conflict left unanswered when the run ends changes nothing
s="$pre;click:alpha.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitconflict"
run "$s"; same "$B/alpha.txt" "B alpha (other version)"; [ "$(ls "$B" | wc -l)" -eq 2 ] || fail "an open conflict sheet changed B"
echo "OK: conflict sheet: Enter = Keep both, Escape cancels, Replace via the trash, Skip all, unanswered = untouched"

# ---------------------------------------------------------------------------------------------
name=permanent-delete; fresh
for s in "click:notes.md;key:Shift+Delete;key:Escape;waitops" \
         "click:notes.md;key:Shift+Delete;key:Enter;waitops" \
         "click:notes.md;key:Shift+Delete;confirm:no;waitops" \
         "click:notes.md;cmd:file.delete_permanently;key:Escape;waitops" \
         "click:notes.md;key:Shift+Delete;key:Tab;key:Shift+Tab;key:Enter;waitops" \
         "click:notes.md;key:Shift+Delete" \
         "click:notes.md;key:Shift+Delete;key:Delete;key:Backspace;key:Space;key:Ctrl+Z;waitops" ; do
    fresh; run "$s"; want "$A/notes.md"; [ "$(trash_count)" -eq 0 ] || fail "[$s] touched the trash"
done
fresh
run "click:notes.md;key:Shift+Delete;key:Tab;key:Enter;waitops"
gone "$A/notes.md"; [ "$(trash_count)" -eq 0 ] || fail "a permanent delete went through the trash"
want "$A/alpha.txt"
fresh
run "click:sub;key:Shift+Delete;confirm:yes;waitops"
gone "$A/sub"; want "$A/alpha.txt"
fresh
# permanent delete is not undoable
run "click:notes.md;key:Shift+Delete;confirm:yes;waitops;key:Ctrl+Z;waitops"; gone "$A/notes.md"
echo "OK: permanent delete only after the explicit accept; Cancel / Escape / Enter / unanswered keep the file"

# ---------------------------------------------------------------------------------------------
name=no-trash-dir; fresh
run "click:notes.md;key:Delete;waitops;click:sub;key:Delete;waitops" --no-trash
want "$A/notes.md"; want "$A/sub/inner.txt"
[ ! -d "$HOME/.local/share/Trash" ] && [ ! -d "$XDG_DATA_HOME/Trash" ] || fail "a headless run created a trash"
fresh
# Delete without a trash asks for the permanent delete (focus on Cancel); only confirm:yes removes
run "click:notes.md;key:Delete;waitops;confirm:no;waitops" --no-trash
want "$A/notes.md"
fresh
run "click:notes.md;key:Delete;waitops;key:Enter;waitops" --no-trash
want "$A/notes.md"
fresh
run "click:notes.md;key:Delete;waitops;confirm:yes;waitops" --no-trash
gone "$A/notes.md"
fresh
# restore / empty / the Trash view without a trash change nothing and fail politely
run "click:notes.md;cmd:trash.open;waitops;cmd:trash.restore;waitops;cmd:trash.empty;waitops" --no-trash
want "$A/notes.md"
echo "OK: headless without --trash-dir: nothing is trashed, no trash is created, permanent delete needs confirm:yes"

# ---------------------------------------------------------------------------------------------
name=quit-running; fresh
mkdir -p "$A/big"
i=0; while [ $i -lt 50 ]; do printf 'file %s\n' $i >"$A/big/f$i.txt"; i=$((i + 1)); done
export NAVIGATOR_DEBUG_OPS_DELAY_MS=60
pre="cmd:pane.split_right;go:$B;focus:0"
# default: keep working. The sheet opens, Enter keeps; the copy finishes whole.
s="$pre;click:big;cmd:edit.copy;focus:1;cmd:edit.paste;cmd:app.quit;key:Enter;waitops;dump:$root/d5.txt"
FRAMES=20 run "$s"
cnt=$(ls "$B/big" 2>/dev/null | wc -l)
[ "$cnt" -eq 50 ] || fail "quit sheet default did not keep working: $cnt of 50 files"
for k in 0 17 49; do same "$B/big/f$k.txt" "file $k"; done
fresh; mkdir -p "$A/big"; i=0; while [ $i -lt 50 ]; do printf 'file %s\n' $i >"$A/big/f$i.txt"; i=$((i + 1)); done
# Escape on the quit sheet also keeps working
s="$pre;click:big;cmd:edit.copy;focus:1;cmd:edit.paste;cmd:app.quit;key:Escape;waitops"
FRAMES=20 run "$s"; [ "$(ls "$B/big" 2>/dev/null | wc -l)" -eq 50 ] || fail "Escape on the quit sheet quit anyway"
fresh; mkdir -p "$A/big"; i=0; while [ $i -lt 50 ]; do printf 'file %s\n' $i >"$A/big/f$i.txt"; i=$((i + 1)); done
# cancel and quit: whatever was copied is whole, no .part, the source is intact, the app exits 0
s="$pre;click:big;cmd:edit.copy;focus:1;cmd:edit.paste;cmd:app.quit;key:Tab;key:Tab;key:Enter;waitops"
FRAMES=200 run "$s"
[ "$(ls "$A/big" | wc -l)" -eq 50 ] || fail "cancel-and-quit damaged the source"
if [ -d "$B/big" ]; then
    for f in "$B"/big/*; do
        [ -e "$f" ] || continue
        k=$(basename "$f"); cmp -s "$f" "$A/big/$k" || fail "truncated or wrong copy after cancel-and-quit: $k"
    done
fi
unset NAVIGATOR_DEBUG_OPS_DELAY_MS
echo "OK: quit while an operation runs asks; default keeps working; cancel-and-quit leaves no .part and no truncated file"

# ---------------------------------------------------------------------------------------------
name=focus-scoping; fresh
pre="cmd:pane.split_right;go:$B;focus:0"
# Chords a text field must keep for itself, then a Tab + Enter probe: had Shift+Delete opened the
# permanent-delete sheet, Tab + Enter would accept it and remove the selected file.
keys="key:Delete;key:Shift+Delete;key:F2;key:Ctrl+Z;key:Ctrl+Y;key:Ctrl+Shift+Z;key:Ctrl+X;key:Ctrl+C;key:Ctrl+V;key:Ctrl+Shift+N;key:F7;key:Shift+F7"
probe="key:Tab;key:Enter"
# One earlier operation gives Ctrl+Z / Ctrl+Y something to undo / redo.
base="$pre;click:delta.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitops;focus:0;click:notes.md"
check_untouched() {
    want "$A/notes.md"; same "$A/notes.md" notes; same "$B/delta.txt" "A delta"
    [ "$(trash_count)" -eq 0 ] || fail "[$1] the trash changed: $(ls "$T/files")"
    [ "$(ls "$B" | wc -l)" -eq 3 ] || fail "[$1] B changed: $(ls "$B")"
    [ "$(ls "$A" | wc -l)" -eq 7 ] || fail "[$1] A changed: $(ls "$A")"
}
# address bar open: the keys belong to the field; F7 / Ctrl+Shift+N / F2 are guarded by the
# focus scoping (no selection acted upon while the address bar has the keyboard)
run "$base;key:Ctrl+L;$keys;$probe;key:Escape;waitops"
check_untouched address-bar
fresh
# filter field focused: its editing chords (Delete, Shift+Delete, Ctrl+V, Ctrl+Z, Ctrl+Y,
# Ctrl+Shift+Z) are text edits. (Ctrl+C / Ctrl+X without a text selection copy / cut the FILES by
# design, and F2 / F7 / Ctrl+Shift+N are not editing chords: documented in docs/keymap.md.)
run "$base;filter:no-such-name-xyz;key:Delete;key:Shift+Delete;key:Ctrl+Z;key:Ctrl+Y;key:Ctrl+Shift+Z;key:Ctrl+V;blurfilter;waitops"
check_untouched filter
fresh
# sidebar focused: edit.* / file.* are not routed to the pane
run "$base;sbfocus:0;key:Delete;key:Shift+Delete;key:Tab;key:Enter;key:Ctrl+X;key:Ctrl+C;key:Ctrl+V;key:F7;key:Shift+F7;key:Ctrl+Shift+N;waitops"
want "$A/notes.md"; same "$B/delta.txt" "A delta"; [ "$(trash_count)" -eq 0 ] || fail "[sidebar] the trash changed"
[ "$(ls "$B" | wc -l)" -eq 3 ] || fail "[sidebar] B changed: $(ls "$B")"
[ ! -e "$A/New folder" ] || fail "[sidebar] Ctrl+Shift+N created a folder while the sidebar had focus"
fresh
# inline rename editor open: F2 again must not restart it (typed text survives), Delete and
# Shift+Delete edit the TEXT, Ctrl+Z is text undo; Enter commits what is shown.
run "$base;rename:gamma.txt;type:zzz;key:F2;key:Enter;waitops"
# F2 inside the open editor is ignored (BUG-P7-1, fixed): the typed text survives and commits.
want "$A/zzz.txt"; gone "$A/gamma.txt"
fresh
run "$base;rename:gamma.txt;type:abc;key:Shift+Delete;key:Delete;key:Ctrl+Z;key:Ctrl+Y;key:Ctrl+Shift+Z;key:Escape;waitops"
want "$A/gamma.txt"; [ ! -e "$A/abc.txt" ] || fail "inline rename: Escape did not cancel"
[ "$(trash_count)" -eq 0 ] || fail "[inline rename] Delete / Shift+Delete reached the files"
want "$A/notes.md"; same "$B/delta.txt" "A delta"
fresh
# Ctrl+Shift+N creates the folder and opens its editor; Escape keeps the default name.
run "$base;key:Ctrl+Shift+N;waitops;key:Escape;waitops"
want "$A/New folder"
fresh
run "$base;key:Ctrl+Shift+N;waitops;type:Projeler ğü;key:Enter;waitops"
want "$A/Projeler ğü"; gone "$A/New folder"
echo "OK: no file command fires while typing in the address bar, the filter, the rename editor or with the sidebar focused"

echo
if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "ops_app_smoke: all checks passed ($n app runs, $xfails known bug(s))"
exit 0
