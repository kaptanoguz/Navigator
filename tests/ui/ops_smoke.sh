#!/bin/sh
# Phase 7 acceptance smoke (owner: ui): a scripted copy, move, undo, trash, restore, conflict
# decision, rename, new folder and confirmed permanent delete on FIXTURES, with a fixture trash
# (--trash-dir), a temporary HOME, --config-dir and --cache-dir, then the filesystem is checked.
# A second run WITHOUT --trash-dir proves a headless run never reaches a trash: trashing an item
# fails (Unsupported) and the file stays where it was.
# Usage: ops_smoke.sh <navigator binary>. Everything lives in a mktemp -d directory.
set -u
bin=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-ops-smoke-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export HOME="$root/home" XDG_CONFIG_HOME="$root/cfg" XDG_CACHE_HOME="$root/cache" XDG_DATA_HOME="$root/data"
mkdir -p "$root/home" "$root/cfg" "$root/cache" "$root/data" "$root/trash" "$root/A/sub" "$root/B"
for n in alpha.txt beta.txt gamma.txt delta.txt notes.md; do printf 'content of %s\n' "$n" >"$root/A/$n"; done
printf 'x' >"$root/A/sub/inner.txt"
printf 'other version\n' >"$root/B/alpha.txt"
A=$root/A
B=$root/B

fail() { echo "FAIL: $1"; cat "$root/run.log" 2>/dev/null | tail -20; exit 1; }
want() { [ -e "$1" ] || fail "missing $1"; }
gone() { [ ! -e "$1" ] || fail "still there: $1"; }

# One scripted session: two panes (A on the left, B on the right).
script="cmd:pane.split_right;go:$B;focus:0"
# copy gamma to B
script="$script;click:gamma.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitops"
# move beta to B (cut / paste), then undo: it comes back
script="$script;focus:0;click:beta.txt;cmd:edit.cut;focus:1;cmd:edit.paste;waitops;dump:$root/moved.txt"
script="$script;cmd:edit.undo;waitops"
# trash notes.md, restore it from the Trash view
script="$script;focus:0;click:notes.md;cmd:file.trash;waitops;cmd:trash.open;waitops;trashclick:notes.md;cmd:trash.restore;waitops;dump:$root/restored.txt"
# conflict: copy alpha.txt onto B/alpha.txt, keep both
script="$script;cmd:nav.back;focus:0;click:alpha.txt;cmd:edit.copy;focus:1;cmd:edit.paste;waitconflict;conflict:keep;waitops"
# rename delta.txt -> renamed.txt (the stem is selected: typing replaces it, the extension stays)
script="$script;focus:0;rename:delta.txt;type:renamed;key:Enter;waitops"
# new folder, named by typing
script="$script;cmd:file.new_folder;waitops;type:Docs;key:Enter;waitops"
# permanent delete needs the confirm sheet: answer no first, then yes
script="$script;click:gamma.txt;cmd:file.delete_permanently;confirm:no;waitops"
script="$script;cmd:file.delete_permanently;confirm:yes;waitops;dump:$root/final.txt"

if ! "$bin" --frames 40 --no-session --no-index --config-dir "$root/cfg" --cache-dir "$root/cache" \
    --trash-dir "$root/trash" --debug-script "$script" "$A" >"$root/run.log" 2>&1; then
    fail "run exited non-zero"
fi
# copy
want "$B/gamma.txt"
# the move happened and the undo brought beta back to A
want "$A/beta.txt"; gone "$B/beta.txt"
# trash and restore
want "$A/notes.md"
[ -z "$(ls "$root/trash/files" 2>/dev/null)" ] || fail "trash not empty after restore: $(ls "$root/trash/files")"
# conflict: both kept
want "$B/alpha.txt"; want "$B/alpha (2).txt"
grep -q 'other version' "$B/alpha.txt" || fail "B/alpha.txt was overwritten"
# rename (extension kept) and new folder
want "$A/renamed.txt"; gone "$A/delta.txt"
want "$A/Docs"
# the confirmed permanent delete removed gamma from A only
gone "$A/gamma.txt"; want "$B/gamma.txt"
echo "OK: copy, move, undo, trash, restore, conflict, rename, new folder, confirmed delete"

# Headless without --trash-dir: no trash exists, so trashing never moves anything.
mkdir -p "$root/home/.local/share"
script="click:sub;cmd:file.trash;waitops"
"$bin" --frames 20 --no-session --no-index --config-dir "$root/cfg" --cache-dir "$root/cache" \
    --debug-script "$script" "$A" >"$root/run2.log" 2>&1 || { cat "$root/run2.log"; fail "run 2 exited non-zero"; }
want "$A/sub/inner.txt"
[ ! -d "$root/home/.local/share/Trash" ] && [ ! -d "$root/data/Trash" ] || fail "a headless run created a trash"
echo "OK: headless run without --trash-dir has no trash"
