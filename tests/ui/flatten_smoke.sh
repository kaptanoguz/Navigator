#!/bin/sh
# Phase 5 acceptance smoke (owner: ui): flatten on a generated tree of a few hundred thousand
# entries can be cancelled and never freezes the UI.
#   navigator --bench-flatten N generates the tree under TMPDIR, flattens it, types a query
#   mid-walk, cancels, waits for the walk thread to exit, leaves the view and quits. It never
#   indexes, and it runs with a temporary HOME, config and cache dir on the dummy video driver.
# Asserts: exit 0, the walk was cancelled mid-way, frames kept coming, the longest loop turn
#   stayed under MAX_FRAME_MS, the walk thread exited within MAX_CANCEL_MS, nothing was cached.
# Usage: flatten_smoke.sh <navigator binary> [entries]. Bounds are generous (debug builds, busy
# machines) and can be overridden in the environment.
set -u
bin=$1
entries=${2:-300000}
max_frame=${MAX_FRAME_MS:-400}
max_cancel=${MAX_CANCEL_MS:-2000}
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-flatten-smoke-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export HOME="$root/home"
export TMPDIR="$root"
unset XDG_CONFIG_HOME XDG_CACHE_HOME DISPLAY WAYLAND_DISPLAY
mkdir -p "$HOME" "$root/cfg" "$root/cache"

if ! "$bin" --bench-flatten "$entries" --config-dir "$root/cfg" --cache-dir "$root/cache" \
    >"$root/out.log" 2>&1; then
    echo "FAIL: navigator exited non-zero"; cat "$root/out.log"; exit 1
fi
value() {
    sed -n "s/^bench-flatten:.* $1=\([-0-9.]*\).*/\1/p; s/^bench-flatten: $1=\([-0-9.]*\).*/\1/p" \
        "$root/out.log" | head -n 1
}
fail() {
    echo "FAIL: $1"; cat "$root/out.log"; exit 1
}
frames=$(value frames)
frame=$(value frame_max_ms)
cancel=$(value cancel_to_exit_ms)
left=$(value threads_left)
[ -n "$frames" ] && [ -n "$frame" ] && [ -n "$cancel" ] || fail "no benchmark report"
[ "$(value cancelled_midwalk)" = 1 ] || fail "the walk ended before it could be cancelled"
[ "$left" = 0 ] || fail "the walk thread did not exit"
[ "$frames" -ge 5 ] || fail "only $frames frames during the walk"
awk "BEGIN { exit !($frame <= $max_frame) }" || fail "frame of $frame ms > $max_frame ms"
awk "BEGIN { exit !($cancel >= 0 && $cancel <= $max_cancel) }" ||
    fail "cancel took $cancel ms (limit $max_cancel)"
[ ! -e "$root/cache/index" ] || fail "an index was written"
grep "^bench-flatten" "$root/out.log"
echo "OK: flatten of $entries entries streams, cancels and keeps rendering"
