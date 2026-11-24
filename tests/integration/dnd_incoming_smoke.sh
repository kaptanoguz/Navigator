#!/bin/sh
# Phase 7 INCOMING drop, protocol level (owner: qa): our own XDND source (xdnd_sender) drops
# files on a real Navigator window under a private Xvfb. Nothing real: temporary HOME / XDG dirs,
# --config-dir, --cache-dir, --trash-dir, no display :0. A drop is a COPY (SDL gives no action
# negotiation): the source files stay, copies appear in the active folder, Turkish names survive,
# a name that already exists becomes a conflict (nothing overwritten while it is open).
# Usage: dnd_incoming_smoke.sh <navigator> <xdnd_sender>. Exit 77 when xvfb / xdotool are missing.
set -u
bin=$1
snd=$2
command -v xvfb-run >/dev/null 2>&1 || { echo "SKIP: xvfb-run missing"; exit 77; }
command -v xdotool >/dev/null 2>&1 || { echo "SKIP: xdotool missing"; exit 77; }
if [ -z "${NAVIGATOR_DND_INNER:-}" ]; then
    NAVIGATOR_DND_INNER=1 exec xvfb-run -a -s "-screen 0 1200x700x24" sh "$0" "$bin" "$snd"
fi
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-dnd-in-XXXXXX") || exit 1
trap 'kill $NP 2>/dev/null; rm -rf "$root"' EXIT INT TERM
export HOME="$root/home" XDG_CONFIG_HOME="$root/cfg" XDG_CACHE_HOME="$root/cache" XDG_DATA_HOME="$root/data"
export SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software
unset WAYLAND_DISPLAY
mkdir -p "$root/home" "$root/cfg" "$root/cache" "$root/trash" "$root/dest" "$root/src"
printf one >"$root/src/alpha.txt"
printf two >"$root/src/Çalışma ğ.txt"
printf theirs >"$root/dest/alpha.txt"
NP=0
"$bin" --frames 600 --no-session --no-index --no-thumbnails --lang en --size 800x500 \
    --config-dir "$root/cfg" --cache-dir "$root/cache" --trash-dir "$root/trash" \
    "$root/dest" >"$root/nav.log" 2>&1 &
NP=$!
wid=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    wid=$(xdotool search --name '^Navigator' 2>/dev/null | head -1)
    [ -n "$wid" ] && break
    sleep 0.5
done
[ -n "$wid" ] || { echo "FAIL: no Navigator window"; exit 1; }
xdotool windowmove "$wid" 0 0 2>/dev/null
sleep 2
"$snd" "$wid" 450 250 "$root/src/Çalışma ğ.txt" >"$root/snd.log" 2>&1
rc=$?
sleep 2
if [ $rc -ne 0 ]; then echo "FAIL: the window did not accept the XDND drop: $(cat "$root/snd.log")"; exit 1; fi
[ "$(cat "$root/dest/Çalışma ğ.txt" 2>/dev/null)" = two ] || { echo "FAIL: dropped file not copied: $(ls "$root/dest")"; tail -5 "$root/nav.log"; exit 1; }
[ -e "$root/src/Çalışma ğ.txt" ] || { echo "FAIL: the source vanished (a drop must copy)"; exit 1; }
[ "$(cat "$root/dest/alpha.txt")" = theirs ] || { echo "FAIL: an existing file changed"; exit 1; }
# A second drop of a name that exists: conflict, nothing overwritten while it is open.
"$snd" "$wid" 450 250 "$root/src/alpha.txt" >"$root/snd2.log" 2>&1
sleep 2
[ "$(cat "$root/dest/alpha.txt")" = theirs ] || { echo "FAIL: a dropped file overwrote an existing one"; exit 1; }
[ -z "$(find "$root/home" "$root/data" -mindepth 1 2>/dev/null | head -1)" ] || { echo "FAIL: wrote under the temporary HOME"; exit 1; }
echo "OK: incoming XDND drop copies, keeps the source, conflicts never overwrite"
exit 0
