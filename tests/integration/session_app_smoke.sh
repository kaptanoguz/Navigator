#!/bin/sh
# Phase 3 headless app (owner: qa): the session survives a restart, on SDL's dummy video driver.
#   1. run 1 (--config-dir C, PATH alpha): split right, split down, new tab, navigate; dump.
#      session.json: 3 panes, 4 tabs with the folders visited, mode 0600, no temp files;
#      recents.json (Phase 4) holds the folders navigated to, mode 0600; no bookmarks.json.
#   2. run 2 (same C, no PATH): restores 3 panes; the dump equals run 1's; session.json has the
#      same panes and folders.
#   3. run 3 (same C, PATH epsilon): the session is kept, epsilon opens as a new active tab.
#   4. a corrupt session.json: exit 0, one pane, the file moved to session.json.bad, a fresh
#      valid session.json written on exit.
#   5. --no-session: an existing session is neither restored nor changed; a fresh config dir
#      gets no session.json (nor bookmarks.json / recents.json).
#   6. a session whose folders were deleted: exit 0, 3 panes still restored.
# Usage: session_app_smoke.sh <navigator binary> [frames]
# Everything lives in a mktemp -d directory that is removed on exit. Nothing is activated.
set -u
bin=$1
frames=${2:-20}
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-session-app-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
fail=0
fx=$root/fx
cfg=$root/cfg
mkdir -p "$fx/alpha" "$fx/beta" "$fx/gamma" "$fx/delta" "$fx/epsilon" "$root/cfg"
: >"$fx/alpha/a.txt"

say_fail() { # message, log file
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

panes_in() { grep -c '"type": "pane"' "$1"; }
# Folder paths of the tabs in a session file, in file order (string paths only).
paths_in() { grep -o '"path": "[^"]*"' "$1"; }

# ---- 1. three panes ----------------------------------------------------------------------------
script="cmd:pane.split_right;go:$fx/beta;cmd:pane.split_down;go:$fx/gamma;cmd:tab.new;go:$fx/delta;dump:$root/dump1.txt"
if run run1 --config-dir "$cfg" --debug-script "$script" "$fx/alpha"; then
    s=$cfg/session.json
    if [ ! -s "$s" ]; then
        say_fail "run1: no session.json" "$root/run1.log"
    else
        [ "$(panes_in "$s")" -eq 3 ] || say_fail "run1: session.json does not hold 3 panes" "$s"
        for d in alpha beta gamma delta; do
            grep -q "\"path\": \"$fx/$d\"" "$s" || say_fail "run1: $d missing from session.json" "$s"
        done
        [ "$(paths_in "$s" | wc -l)" -eq 4 ] || say_fail "run1: expected 4 tabs" "$s"
        mode=$(stat -c %a "$s")
        [ "$mode" = 600 ] || say_fail "run1: session.json mode $mode, expected 600"
        # Phase 4: run 1 navigates, so recents.json is a legitimate neighbour (bookmarks.json
        # only when a bookmark changed, which this script never does). Anything else (temp
        # files, .bad) is a leftover.
        extra=$(ls -A "$cfg" | grep -Ev '^(session|recents)\.json$')
        [ -z "$extra" ] || say_fail "run1: leftover files in the config dir: $extra"
        [ ! -e "$cfg/bookmarks.json" ] || say_fail "run1: bookmarks.json without a bookmark edit"
        if [ -e "$cfg/recents.json" ]; then
            mode=$(stat -c %a "$cfg/recents.json")
            [ "$mode" = 600 ] || say_fail "run1: recents.json mode $mode, expected 600"
            for d in beta gamma delta; do
                grep -q "\"$fx/$d\"" "$cfg/recents.json" ||
                    say_fail "run1: $d missing from recents.json" "$cfg/recents.json"
            done
        else
            say_fail "run1: navigation wrote no recents.json"
        fi
        cp "$s" "$root/session1.json"
    fi
    grep -q '^panes 3$' "$root/dump1.txt" || say_fail "run1: dump is not 3 panes" "$root/dump1.txt"
fi

# ---- 2. restore --------------------------------------------------------------------------------
if run run2 --config-dir "$cfg" --debug-script "dump:$root/dump2.txt"; then
    grep -q '^panes 3$' "$root/dump2.txt" || say_fail "run2: restored layout is not 3 panes" \
        "$root/dump2.txt"
    cmp -s "$root/dump1.txt" "$root/dump2.txt" ||
        { echo "FAIL run2: restored layout differs"; diff "$root/dump1.txt" "$root/dump2.txt"; fail=1; }
    [ "$(panes_in "$cfg/session.json")" -eq 3 ] || say_fail "run2: saved session lost panes" \
        "$cfg/session.json"
    [ "$(paths_in "$cfg/session.json")" = "$(paths_in "$root/session1.json")" ] ||
        say_fail "run2: saved tab folders differ from run 1" "$cfg/session.json"
fi

# ---- 3. PATH with a session --------------------------------------------------------------------
cp "$root/session1.json" "$cfg/session.json"
if run run3 --config-dir "$cfg" --debug-script "dump:$root/dump3.txt" "$fx/epsilon"; then
    grep -q '^panes 3$' "$root/dump3.txt" || say_fail "run3: session not kept" "$root/dump3.txt"
    grep -q "^  tab $fx/epsilon live$" "$root/dump3.txt" ||
        say_fail "run3: PATH is not the live tab" "$root/dump3.txt"
    [ "$(grep -c '^  tab ' "$root/dump3.txt")" -eq 5 ] || say_fail "run3: expected 5 tabs" \
        "$root/dump3.txt"
fi

# ---- 4. corrupt session ------------------------------------------------------------------------
printf '{"version": 1, "layout": {"root": {"type": "pa' >"$cfg/session.json"
cp "$cfg/session.json" "$root/corrupt.json"
if run run4 --config-dir "$cfg" --debug-script "dump:$root/dump4.txt" "$fx/beta"; then
    grep -q '^panes 1$' "$root/dump4.txt" || say_fail "run4: not a default layout" "$root/dump4.txt"
    grep -q "^  tab $fx/beta live$" "$root/dump4.txt" || say_fail "run4: start folder not shown" \
        "$root/dump4.txt"
    cmp -s "$root/corrupt.json" "$cfg/session.json.bad" ||
        say_fail "run4: the corrupt file was not moved to session.json.bad"
    if [ -s "$cfg/session.json" ]; then
        [ "$(panes_in "$cfg/session.json")" -eq 1 ] || say_fail "run4: bad new session" \
            "$cfg/session.json"
    else
        say_fail "run4: no fresh session.json written on exit" "$root/run4.log"
    fi
fi

# ---- 5. --no-session ---------------------------------------------------------------------------
cp "$root/session1.json" "$cfg/session.json"
if run run5 --no-session --config-dir "$cfg" --debug-script "cmd:pane.split_right;dump:$root/dump5.txt" "$fx/gamma"; then
    grep -q '^panes 2$' "$root/dump5.txt" || say_fail "run5: --no-session restored the session" \
        "$root/dump5.txt"
    cmp -s "$root/session1.json" "$cfg/session.json" || say_fail "run5: --no-session wrote"
fi
if run run5b --no-session --config-dir "$root/cfg-fresh" "$fx"; then
    [ ! -e "$root/cfg-fresh/session.json" ] || say_fail "run5b: --no-session wrote a session"
    # Phase 4: bookmarks / recents follow the same flags.
    [ -z "$(ls -A "$root/cfg-fresh" 2>/dev/null)" ] ||
        say_fail "run5b: --no-session wrote $(ls -A "$root/cfg-fresh")"
fi

# ---- 6. folders gone ---------------------------------------------------------------------------
cp "$root/session1.json" "$cfg/session.json"
rm -rf "$fx/beta" "$fx/gamma" "$fx/delta"
if run run6 --config-dir "$cfg" --debug-script "cmd:pane.focus_next;cmd:tab.next;dump:$root/dump6.txt"; then
    grep -q '^panes 3$' "$root/dump6.txt" || say_fail "run6: panes lost" "$root/dump6.txt"
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: 3 panes saved and restored; PATH, corrupt file, --no-session, missing folders"
    cat "$root/dump2.txt"
fi
exit $fail
