#!/bin/sh
# Phase 4 acceptance smoke (owner: ui): GoTo jumps to "~/Downloads" and to "/".
#   A temporary HOME holds Downloads; navigator runs headless with a temporary --config-dir,
#   a debug script opens GoTo (nav.goto), types the path, presses Enter (list.activate, routed
#   to the popup) and dumps the panes. The dump must show the active tab on the target.
#   A third run checks that bookmarks.json / recents.json land in --config-dir only.
# Usage: goto_smoke.sh <navigator binary>. Everything lives in a mktemp -d directory.
set -u
bin=$1
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-goto-smoke-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export HOME="$root/home"
unset XDG_CONFIG_HOME
mkdir -p "$HOME/Downloads" "$HOME/start" "$root/cfg"

run() { # run NAME SCRIPT
    if ! "$bin" --frames 10 --config-dir "$root/cfg" --debug-script "$2" "$HOME/start" \
        >"$root/$1.log" 2>&1; then
        echo "FAIL: $1 exited non-zero"; cat "$root/$1.log"; exit 1
    fi
}
run downloads "cmd:nav.goto;type:~/Downloads;cmd:list.activate;dump:$root/d1.txt"
if ! grep -q "^  tab $HOME/Downloads live" "$root/d1.txt"; then
    echo "FAIL: GoTo ~/Downloads did not navigate"; cat "$root/d1.txt"; exit 1
fi
run root "cmd:nav.goto;type:/;cmd:list.activate;dump:$root/d2.txt"
if ! grep -q "^  tab / live" "$root/d2.txt"; then
    echo "FAIL: GoTo / did not navigate"; cat "$root/d2.txt"; exit 1
fi
# Ctrl+D bookmarks the folder; quitting writes bookmarks.json and recents.json to --config-dir.
run bookmark "cmd:bookmarks.toggle;go:$HOME/Downloads;dump:$root/d3.txt"
if ! grep -q '"bookmarks"' "$root/cfg/bookmarks.json" 2>/dev/null ||
    ! grep -q "start" "$root/cfg/bookmarks.json"; then
    echo "FAIL: bookmark not saved"; cat "$root/cfg/bookmarks.json"; exit 1
fi
if ! grep -q "Downloads" "$root/cfg/recents.json" 2>/dev/null; then
    echo "FAIL: recent not saved"; cat "$root/cfg/recents.json"; exit 1
fi
if [ -e "$HOME/.config/navigator" ]; then
    echo "FAIL: wrote under ~/.config"; exit 1
fi
echo "OK: GoTo ~/Downloads and / navigate; bookmarks and recents saved"
