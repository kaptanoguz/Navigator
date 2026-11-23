#!/bin/sh
# Phase 6 acceptance smoke (owner: ui): the Inspector opens on a PNG, a JPEG, a WebP, a text
# file and a folder, a 100 MP-claimed corrupt PNG fails cleanly, and a huge single-line text file
# does not stall frames. Headless (dummy video, software renderer), temporary HOME,
# --config-dir, --cache-dir and --thumbnail-dir; never the user's ~/.cache/thumbnails, never an
# external thumbnailer.
# Usage: inspector_smoke.sh <navigator binary> <fixtures dir> [max frame ms]
set -u
bin=$1
fixtures=$2
bound=${3:-250}
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-inspector-smoke-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export HOME="$root/home"
unset XDG_CACHE_HOME XDG_CONFIG_HOME
data="$root/data"
mkdir -p "$HOME" "$data/folder/sub" "$root/cfg" "$root/cache" "$root/thumbs"
cp "$fixtures/alpha.png" "$fixtures/photo.jpg" "$fixtures/sample.webp" "$data/"
cp "$fixtures/huge-corrupt.png" "$data/huge.png"
printf 'Navigator\nInspector smoke\n\tindented line\n' >"$data/notes.txt"
touch "$data/folder/a.txt" "$data/folder/b.txt" "$data/folder/.hidden"
# 8 MiB on one line: the preview reads the first 4 MiB (4096 rows of 1 KiB).
head -c 8388608 /dev/zero | tr '\0' 'a' >"$data/one-line.txt"

fail() { echo "FAIL: $1"; [ -n "${2:-}" ] && cat "$2"; exit 1; }

run() { # run NAME SCRIPT [extra args...]
    name=$1
    script=$2
    shift 2
    if ! "$bin" --frames 20 --config-dir "$root/cfg" --cache-dir "$root/cache" \
        --thumbnail-dir "$root/thumbs" --no-index --no-external-thumbnailers --no-session \
        --debug-script "$script" "$@" "$data" >"$root/$name.log" 2>&1; then
        fail "$name exited non-zero" "$root/$name.log"
    fi
}

check() { # check NAME PATTERN
    grep -q "$2" "$root/$1.txt" || { cat "$root/$1.log"; fail "$1: expected '$2'" "$root/$1.txt"; }
}

for f in alpha.png photo.jpg sample.webp; do
    run "$f" "inspect:$f;waitpreview;dump:$root/$f.txt"
    check "$f" "^inspector visible placement right status ready kind image item $f"
done
run text "inspect:notes.txt;waitpreview;dump:$root/text.txt"
check text "status ready kind text item notes.txt"
run folder "inspect:folder;waitpreview;dump:$root/folder.txt"
check folder "status ready kind folder item folder"
# 1:1 and the toggles run on an image without crashing; placement cycles to bottom.
run zoom "inspect:photo.jpg;waitpreview;cmd:inspector.zoom_1to1;cmd:inspector.toggle_grid;cmd:inspector.toggle_lock;cmd:inspector.cycle_placement;cmd:inspector.cycle_placement;waitpreview;dump:$root/zoom.txt"
check zoom "placement bottom status ready kind image item photo.jpg locked 1 grid 1"
# A corrupt file claiming 100 MP: an error (or placeholder), never a crash or a huge allocation.
run corrupt "inspect:huge.png;waitpreview;dump:$root/corrupt.txt"
grep -q "item huge.png" "$root/corrupt.txt" || fail "corrupt: not inspected" "$root/corrupt.txt"
grep -Eq "status (failed|ready kind placeholder)" "$root/corrupt.txt" ||
    fail "corrupt: expected a failure state" "$root/corrupt.txt"
# The huge single line: frames stay bounded while the Inspector scrolls through it.
run huge "inspect:one-line.txt;waitpreview;inspectorfocus;cmd:list.cursor_page_down;cmd:list.cursor_page_down;cmd:list.cursor_end;dump:$root/huge.txt"
check huge "status ready kind text item one-line.txt"
max=$(sed -n 's/^inspector_max_frame_ms //p' "$root/huge.txt")
awk -v m="$max" -v b="$bound" 'BEGIN { exit !(m + 0 < b + 0) }' ||
    fail "huge text: a frame took ${max} ms (bound ${bound} ms)" "$root/huge.txt"
# Thumbnails went to --thumbnail-dir only.
if [ -e "$HOME/.cache/thumbnails" ]; then
    fail "wrote to ~/.cache/thumbnails"
fi
# Headless without --thumbnail-dir / --cache-dir: no disk cache at all.
if ! "$bin" --frames 10 --no-session --no-index --debug-script "inspect:photo.jpg;waitpreview" \
    "$data" >"$root/nodirs.log" 2>&1; then
    fail "nodirs exited non-zero" "$root/nodirs.log"
fi
if [ -e "$HOME/.cache/thumbnails" ] || [ -e "$HOME/.cache/navigator" ]; then
    fail "headless run without dirs touched ~/.cache"
fi
echo "OK: Inspector previews PNG/JPEG/WebP/text/folder, fails cleanly on a corrupt 100 MP PNG, huge line max frame ${max} ms"
