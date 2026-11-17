#!/bin/sh
# Phase 3 acceptance smoke (owner: ui): a 3-pane layout survives a restart.
#   run 1: --config-dir TMP, scripted: split right, new tab, split down, navigate; exits after
#          the frames (the session is written synchronously on exit) and dumps the layout;
#   run 2: same --config-dir, no PATH: restores, dumps again; both dumps must be equal and the
#          session.json must hold three panes.
# Usage: session_smoke.sh <navigator binary>. Everything lives in a mktemp -d directory.
set -u
bin=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-session-smoke-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
mkdir -p "$root/cfg" "$root/fx/alpha" "$root/fx/beta" "$root/fx/gamma" "$root/fx/delta"
: >"$root/fx/alpha/a.txt"
fx=$root/fx

script="cmd:pane.split_right;go:$fx/beta;cmd:tab.new;go:$fx/gamma;cmd:pane.split_down;go:$fx/delta;cmd:pane.focus_next;dump:$root/dump1.txt"
if ! "$bin" --frames 20 --config-dir "$root/cfg" --debug-script "$script" "$fx/alpha" \
    >"$root/run1.log" 2>&1; then
    echo "FAIL: run 1 exited non-zero"; cat "$root/run1.log"; exit 1
fi
if [ ! -s "$root/cfg/session.json" ]; then
    echo "FAIL: no session.json after run 1"; cat "$root/run1.log"; exit 1
fi
panes=$(grep -c '"type": "pane"' "$root/cfg/session.json")
if [ "$panes" -ne 3 ]; then
    echo "FAIL: session.json has $panes panes, expected 3"; cat "$root/cfg/session.json"; exit 1
fi
if ! "$bin" --frames 20 --config-dir "$root/cfg" --debug-script "dump:$root/dump2.txt" \
    >"$root/run2.log" 2>&1; then
    echo "FAIL: run 2 exited non-zero"; cat "$root/run2.log"; exit 1
fi
if ! grep -q '^panes 3$' "$root/dump2.txt"; then
    echo "FAIL: restored layout is not 3 panes"; cat "$root/dump2.txt"; exit 1
fi
if ! cmp -s "$root/dump1.txt" "$root/dump2.txt"; then
    echo "FAIL: restored layout differs"; diff "$root/dump1.txt" "$root/dump2.txt"; exit 1
fi
# --no-session never writes.
rm -rf "$root/cfg2"
"$bin" --frames 5 --no-session --config-dir "$root/cfg2" "$fx" >/dev/null 2>&1
if [ -e "$root/cfg2/session.json" ]; then
    echo "FAIL: --no-session wrote a session"; exit 1
fi
echo "OK: 3 panes restored"
cat "$root/dump2.txt"
