#!/bin/sh
# Phase 4 headless app (owner: qa): the Phase 4 chords through the real key path
# (--debug-script key:CHORD -> handle_key -> keymap -> App), bookmarks on a fake volume table
# across restarts (NAVIGATOR_DEBUG_VOLUMES), and the headless no-write rule.
#   1. key:Ctrl+B hides the sidebar (session.json "visible": false); a second run's Ctrl+B
#      shows it again.
#   2. key:Ctrl+D bookmarks the active folder (bookmarks.json); Ctrl+D again removes it.
#   3. key:Ctrl+L, type a path, key:Enter: the address bar navigates the active tab.
#   4. key:Ctrl+G, type "~/downloads" (wrong case), key:Enter: GoTo lands on ~/Downloads;
#      Ctrl+G + "~/indirilenler" lands on ~/İndirilenler; key:Ctrl+Enter opens a new tab.
#   5. key:Ctrl+Shift+C runs (the clipboard is not observable on the dummy driver: exit 0 only).
#   6. Fake volumes: a bookmark on the stick (uuid 1234-ABCD) is saved with its identity; a run
#      without the stick keeps it (file unchanged); a run with the stick at another mount point
#      rebases it in bookmarks.json.
#   7. --frames without --config-dir, HOME and XDG_CONFIG_HOME in a temporary dir, a script
#      that navigates, bookmarks and toggles the sidebar: nothing is created under either.
# Usage: places_app_smoke.sh <navigator binary> [frames]
# Everything lives in a mktemp -d directory that is removed on exit. Nothing is activated.
set -u
bin=$1
frames=${2:-30}
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-places-app-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export HOME="$root/home"
export XDG_CONFIG_HOME="$root/xdg-config"
unset NAVIGATOR_DEBUG_VOLUMES
fail=0
cfg=$root/cfg
fx=$root/fx
mkdir -p "$HOME/Downloads" "$HOME/İndirilenler" "$fx/alpha" "$fx/beta" "$cfg" "$XDG_CONFIG_HOME"

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

# ---- 1. Ctrl+B ---------------------------------------------------------------------------------
if run sidebar1 --config-dir "$cfg" --debug-script "key:Ctrl+B;dump:$root/d1.txt" "$fx/alpha"; then
    grep -A2 '"sidebar"' "$cfg/session.json" | grep -q '"visible": false' ||
        say_fail "sidebar1: Ctrl+B did not hide the sidebar" "$cfg/session.json"
fi
if run sidebar2 --config-dir "$cfg" --debug-script "key:Ctrl+B;dump:$root/d1b.txt"; then
    grep -A2 '"sidebar"' "$cfg/session.json" | grep -q '"visible": true' ||
        say_fail "sidebar2: Ctrl+B did not show the sidebar again" "$cfg/session.json"
fi

# ---- 2. Ctrl+D ---------------------------------------------------------------------------------
rm -f "$cfg/session.json"
if run bm1 --config-dir "$cfg" --debug-script "key:Ctrl+D;dump:$root/d2.txt" "$fx/beta"; then
    grep -q "\"$fx/beta\"" "$cfg/bookmarks.json" 2>/dev/null ||
        say_fail "bm1: Ctrl+D did not bookmark the folder" "$cfg/bookmarks.json"
    mode=$(stat -c %a "$cfg/bookmarks.json" 2>/dev/null)
    [ "$mode" = 600 ] || say_fail "bm1: bookmarks.json mode $mode, expected 600"
fi
if run bm2 --config-dir "$cfg" --debug-script "key:Ctrl+D;dump:$root/d2b.txt"; then
    if grep -q "\"$fx/beta\"" "$cfg/bookmarks.json" 2>/dev/null; then
        say_fail "bm2: a second Ctrl+D did not remove the bookmark" "$cfg/bookmarks.json"
    fi
fi

# ---- 3. Ctrl+L ---------------------------------------------------------------------------------
rm -f "$cfg/session.json"
if run addr --config-dir "$cfg" \
    --debug-script "key:Ctrl+L;type:$fx/alpha;key:Enter;dump:$root/d3.txt" "$fx/beta"; then
    grep -q "^  tab $fx/alpha live$" "$root/d3.txt" ||
        say_fail "addr: the address bar did not navigate" "$root/d3.txt"
    [ "$(grep -c '^  tab ' "$root/d3.txt")" -eq 1 ] || say_fail "addr: expected 1 tab" "$root/d3.txt"
fi

# ---- 4. Ctrl+G ---------------------------------------------------------------------------------
rm -f "$cfg/session.json"
if run goto --config-dir "$cfg" --debug-script \
    "key:Ctrl+G;type:~/downloads;key:Enter;dump:$root/d4.txt;key:Ctrl+G;type:~/indirilenler;key:Enter;dump:$root/d4b.txt;key:Ctrl+G;type:$fx/alpha;key:Ctrl+Enter;dump:$root/d4c.txt" \
    "$fx/beta"; then
    grep -q "^  tab $HOME/Downloads live$" "$root/d4.txt" ||
        say_fail "goto: ~/downloads did not land on ~/Downloads" "$root/d4.txt"
    grep -q "^  tab $HOME/İndirilenler live$" "$root/d4b.txt" ||
        say_fail "goto: ~/indirilenler did not land on ~/İndirilenler" "$root/d4b.txt"
    [ "$(grep -c '^  tab ' "$root/d4c.txt")" -eq 2 ] ||
        say_fail "goto: Ctrl+Enter did not open a second tab" "$root/d4c.txt"
    grep -q "^  tab $fx/alpha$" "$root/d4c.txt" ||
        say_fail "goto: the new tab is not a background tab on alpha" "$root/d4c.txt"
    grep -q "\"$HOME/Downloads\"" "$cfg/recents.json" 2>/dev/null ||
        say_fail "goto: ~/Downloads not recorded in recents.json" "$cfg/recents.json"
fi

# ---- 5. Ctrl+Shift+C ---------------------------------------------------------------------------
run copy --config-dir "$cfg" --debug-script "key:Ctrl+Shift+C;dump:$root/d5.txt" "$fx/beta" || true

# ---- 6. Bookmarks on a fake volume table ---------------------------------------------------------
vol=$root/vol
stick=$fx/media/STICK
stick2=$fx/media/STICK2
mkdir -p "$stick/data" "$vol/sys" "$vol/dev/disk/by-uuid" "$root/cfg6"
ln -s ../../sdb1 "$vol/dev/disk/by-uuid/1234-ABCD"
table() { # mount point of the stick, or "" for none
    printf '22 1 259:2 / / rw - ext4 /dev/nvme0n1p2 rw\n' >"$vol/mountinfo"
    [ -n "$1" ] && printf '29 22 8:17 / %s rw - vfat /dev/sdb1 rw\n' "$1" >>"$vol/mountinfo"
    return 0
}
table "$stick"
export NAVIGATOR_DEBUG_VOLUMES=$vol
if run vol1 --config-dir "$root/cfg6" --debug-script "dump:$root/v0.txt;key:Ctrl+D;dump:$root/v1.txt" \
    "$stick/data"; then
    b=$root/cfg6/bookmarks.json
    grep -q "\"$stick/data\"" "$b" 2>/dev/null || say_fail "vol1: bookmark not saved" "$b"
    grep -q '1234-ABCD' "$b" 2>/dev/null || say_fail "vol1: no volume identity saved" "$b"
    cp "$b" "$root/vol1.json" 2>/dev/null
fi
# Unmounted: the stick's folder goes away with it; the bookmark stays in the file.
mv "$stick" "$root/parked"
table ""
if run vol2 --config-dir "$root/cfg6" --debug-script "dump:$root/v2.txt" "$fx/alpha"; then
    cmp -s "$root/vol1.json" "$root/cfg6/bookmarks.json" ||
        say_fail "vol2: bookmarks.json changed while the stick was offline" "$root/cfg6/bookmarks.json"
fi
# Back at another mount point: rebased.
mkdir -p "$fx/media"
mv "$root/parked" "$stick2"
table "$stick2"
if run vol3 --config-dir "$root/cfg6" --debug-script "dump:$root/v3.txt;dump:$root/v3b.txt" "$fx/alpha"; then
    grep -q "\"$stick2/data\"" "$root/cfg6/bookmarks.json" ||
        say_fail "vol3: the bookmark was not rebased to STICK2" "$root/cfg6/bookmarks.json"
    if grep -q "\"$stick/data\"" "$root/cfg6/bookmarks.json"; then
        say_fail "vol3: the old path is still stored" "$root/cfg6/bookmarks.json"
    fi
fi
unset NAVIGATOR_DEBUG_VOLUMES

# ---- 7. Headless without --config-dir writes nothing under HOME / XDG_CONFIG_HOME -------------
find "$HOME" "$XDG_CONFIG_HOME" | sort >"$root/before.txt"
if run nocfg --debug-script \
    "go:$fx/alpha;key:Ctrl+D;key:Ctrl+B;key:Ctrl+G;type:$fx/beta;key:Enter;cmd:tab.new;dump:$root/d7.txt" \
    "$fx/beta"; then
    find "$HOME" "$XDG_CONFIG_HOME" | sort >"$root/after.txt"
    cmp -s "$root/before.txt" "$root/after.txt" ||
        { echo "FAIL nocfg: files appeared"; diff "$root/before.txt" "$root/after.txt"; fail=1; }
    grep -q "^  tab $fx/beta live$" "$root/d7.txt" || say_fail "nocfg: script did not run" "$root/d7.txt"
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: Ctrl+B, Ctrl+D, Ctrl+L, Ctrl+G, Ctrl+Shift+C; offline bookmark across restarts; no writes without --config-dir"
fi
exit $fail
