#!/bin/sh
# Phase 5 headless app (owner: qa): the name-index service's file rules in the real binary.
#   1. --frames without --cache-dir (HOME, XDG_CACHE_HOME, XDG_CONFIG_HOME in a temporary dir,
#      NAVIGATOR_DEBUG_INDEX_ROOT set, Ctrl+E and Ctrl+Shift+E pressed): nothing is created
#      under any of them (docs/architecture.md "Phase 5 integration").
#   2. --cache-dir C --no-index: C/index is never created.
#   3. --cache-dir C with NAVIGATOR_DEBUG_INDEX_ROOT=<fixture> (never a real volume): waitindex,
#      Ctrl+Shift+E (index-backed), a filter, Escape; at quit C/index holds exactly one .idx
#      file, mode 600 in a 700 folder, starting with the NAVIDX magic.
#   4. The .idx is corrupted (bytes overwritten mid-file); the next run rebuilds and rewrites
#      it (different bytes, valid magic, same size as the first good file).
# Usage: index_app_smoke.sh <navigator binary> [frames]
# Everything lives in a mktemp -d directory that is removed on exit. Nothing is activated.
set -u
bin=$1
frames=${2:-30}
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-index-app-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export HOME="$root/home"
export XDG_CONFIG_HOME="$root/xdg-config"
export XDG_CACHE_HOME="$root/xdg-cache"
unset NAVIGATOR_DEBUG_VOLUMES DISPLAY WAYLAND_DISPLAY
fail=0
fx=$root/fx
mkdir -p "$HOME" "$fx/alpha/inner" "$fx/beta" "$fx/.hidden"
for f in alpha/a1.txt alpha/inner/report.md beta/b1.txt top.txt .hidden/secret.txt; do
    : >"$fx/$f"
done
export NAVIGATOR_DEBUG_INDEX_ROOT="$fx"

say_fail() { # message, file to show
    echo "FAIL $1"
    [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/    /' "$2"
    fail=1
}

run() { # name, args...
    name=$1
    shift
    "$bin" --frames "$frames" "$@" >"$root/$name.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        say_fail "$name: exit $rc" "$root/$name.log"
        return 1
    fi
    if grep -Eqi 'AddressSanitizer|ThreadSanitizer|runtime error|LeakSanitizer' "$root/$name.log"; then
        say_fail "$name: sanitizer report" "$root/$name.log"
        return 1
    fi
    return 0
}

flatten_script="key:Ctrl+E;waitsearch;filter:re;waitsearch;key:Escape;key:Escape;key:Ctrl+Shift+E;waitsearch;filter:a1;waitsearch;key:Escape;key:Escape"

# ---- 1. headless without --cache-dir writes nothing ------------------------------------------
if run nocache --debug-script "$flatten_script" "$fx"; then
    left=$(find "$HOME" "$root/xdg-config" "$root/xdg-cache" -mindepth 1 2>/dev/null)
    [ -z "$left" ] || say_fail "nocache: files created: $left"
    [ ! -e "$root/xdg-cache" ] || say_fail "nocache: XDG_CACHE_HOME was created"
fi

# ---- 2. --no-index ---------------------------------------------------------------------------
c2=$root/cache-noindex
mkdir -p "$c2"
if run noindex --no-index --cache-dir "$c2" --debug-script "$flatten_script" "$fx"; then
    [ ! -e "$c2/index" ] || say_fail "noindex: $c2/index was created"
fi

# ---- 3. --cache-dir with the fixture as the only indexed root ---------------------------------
c3=$root/cache
mkdir -p "$c3"
script3="waitindex;$flatten_script"
idx=
if run index1 --cache-dir "$c3" --debug-script "$script3" "$fx"; then
    count=$(find "$c3/index" -maxdepth 1 -name '*.idx' 2>/dev/null | wc -l)
    others=$(find "$c3/index" -mindepth 1 ! -name '*.idx' 2>/dev/null)
    if [ "$count" -ne 1 ] || [ -n "$others" ]; then
        say_fail "index1: expected one .idx file, got $count (others: $others)"
    else
        idx=$(find "$c3/index" -maxdepth 1 -name '*.idx')
        [ "$(stat -c %a "$idx")" = 600 ] || say_fail "index1: file mode $(stat -c %a "$idx")"
        [ "$(stat -c %a "$c3/index")" = 700 ] || say_fail "index1: dir mode $(stat -c %a "$c3/index")"
        [ "$(head -c 6 "$idx")" = NAVIDX ] || say_fail "index1: no NAVIDX magic"
    fi
    left=$(find "$HOME" "$root/xdg-cache" -mindepth 1 2>/dev/null)
    [ -z "$left" ] || say_fail "index1: files created outside --cache-dir: $left"
fi

# ---- 4. corrupt cache -> rebuild ------------------------------------------------------------
if [ -n "$idx" ]; then
    size=$(stat -c %s "$idx")
    cp "$idx" "$root/good.idx"
    printf 'CORRUPTCORRUPTCORRUPT' | dd of="$idx" bs=1 seek=$((size / 2)) conv=notrunc 2>/dev/null
    cp "$idx" "$root/bad.idx"
    if run index2 --cache-dir "$c3" --debug-script "$script3" "$fx"; then
        if cmp -s "$idx" "$root/bad.idx"; then
            say_fail "index2: the corrupt file was kept"
        fi
        [ "$(head -c 6 "$idx")" = NAVIDX ] || say_fail "index2: rebuilt file has no magic"
        [ "$(stat -c %s "$idx")" = "$size" ] ||
            say_fail "index2: rebuilt file is $(stat -c %s "$idx") bytes, first one $size"
    fi
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "OK: headless runs never write a cache; --no-index writes none; the index file is" \
    "private, rebuilt after corruption"
