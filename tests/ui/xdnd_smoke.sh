#!/bin/sh
# Phase 7 outgoing drag smoke (owner: ui): a REAL pointer drag (xdotool) from a Navigator row
# to a separate X11 window that speaks XDND (navigator_xdnd_receiver) under a private Xvfb.
# Verifies the protocol path of src/ui/platform/xdnd_source.cpp: XdndEnter / Position / Drop,
# the text/uri-list selection (percent-encoded UTF-8 names), nothing deleted or moved, and that
# releasing outside any target delivers nothing. It does NOT prove GNOME Files or a Wayland
# compositor accept the drag: docs/dnd.md lists what the owner checks by hand.
# Usage: xdnd_smoke.sh <navigator> <receiver>. Needs xvfb-run and xdotool (skipped otherwise).
set -u
bin=$1
recv=$2
command -v xvfb-run >/dev/null 2>&1 || { echo "SKIP: xvfb-run missing"; exit 77; }
command -v xdotool >/dev/null 2>&1 || { echo "SKIP: xdotool missing"; exit 77; }
if [ -z "${NAVIGATOR_XDND_INNER:-}" ]; then
    NAVIGATOR_XDND_INNER=1 exec xvfb-run -a -s "-screen 0 1200x520x24" sh "$0" "$bin" "$recv"
fi
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-xdnd-smoke-XXXXXX") || exit 1
trap 'kill $NP $RP 2>/dev/null; rm -rf "$root"' EXIT INT TERM
export HOME="$root/home" XDG_CONFIG_HOME="$root/cfg" XDG_CACHE_HOME="$root/cache" XDG_DATA_HOME="$root/data"
export SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software
mkdir -p "$root/home" "$root/cfg" "$root/cache" "$root/trash" "$root/A"
printf one >"$root/A/alpha.txt"
printf two >"$root/A/Beta file.txt"
printf three >"$root/A/şarkı.txt"

drag() { # drag NAME SCRIPT END_X : press on the first row, drag right to END_X, release
    name=$1; script=$2; endx=$3
    rm -f "$root/recv.txt"
    "$recv" "$root/recv.txt" 800 20 280 300 >"$root/recv.log" 2>&1 &
    RP=$!
    "$bin" --frames 700 --no-session --no-index --size 720x480 --config-dir "$root/cfg" \
        --cache-dir "$root/cache" --trash-dir "$root/trash" --debug-script "cmd:view.toggle_sidebar;$script" \
        "$root/A" >"$root/nav-$name.log" 2>&1 &
    NP=$!
    wid=""
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
        wid=$(xdotool search --name '^Navigator' 2>/dev/null | head -1)
        [ -n "$wid" ] && break
        sleep 0.5
    done
    [ -n "$wid" ] || { echo "FAIL($name): no Navigator window"; return 1; }
    xdotool windowmove "$wid" 0 0 2>/dev/null
    sleep 1.5
    xdotool mousemove 90 128 mousedown 1
    sleep 0.3
    for x in 150 250 400 550 700 800 880 $endx; do xdotool mousemove "$x" 140; sleep 0.12; done
    sleep 0.4
    xdotool mouseup 1
    sleep 2
    kill $NP 2>/dev/null; kill $RP 2>/dev/null
    wait $NP 2>/dev/null; wait $RP 2>/dev/null
    return 0
}

drag one "click:alpha.txt" 920
[ -s "$root/recv.txt" ] || { echo "FAIL: the receiver got no drop"; cat "$root/recv.log"; tail -5 "$root/nav-one.log"; exit 1; }
grep -q 'alpha.txt' "$root/recv.txt" || { echo "FAIL: wrong uri list"; cat "$root/recv.txt"; exit 1; }
grep -q '^file://' "$root/recv.txt" || { echo "FAIL: not file:// URIs"; exit 1; }
[ "$(wc -l <"$root/recv.txt")" -eq 1 ] || { echo "FAIL: expected one URI"; cat "$root/recv.txt"; exit 1; }
[ -e "$root/A/alpha.txt" ] || { echo "FAIL: the source file vanished"; exit 1; }
echo "OK: single file dropped on an XDND receiver"

# Two selected rows: one drag carries both (Turkish names are percent-encoded).
drag two "click:alpha.txt;click:Beta file.txt:ctrl" 920
n=$(wc -l <"$root/recv.txt" 2>/dev/null || echo 0)
[ "$n" -eq 2 ] || { echo "FAIL: expected two URIs, got $n"; cat "$root/recv.txt" 2>&1; cat "$root/recv.log"; exit 1; }
grep -q 'Beta%20file.txt' "$root/recv.txt" || { echo "FAIL: names are not percent-encoded"; cat "$root/recv.txt"; exit 1; }
echo "OK: two files dropped"

# Released outside every window: nothing is delivered, nothing is deleted.
drag none "click:alpha.txt" 760
[ ! -s "$root/recv.txt" ] || { echo "FAIL: a drop reached the receiver without being over it"; exit 1; }
for f in alpha.txt "Beta file.txt" "şarkı.txt"; do
    [ -e "$root/A/$f" ] || { echo "FAIL: $f is gone"; exit 1; }
done
echo "OK: no drop outside a target, files untouched"
