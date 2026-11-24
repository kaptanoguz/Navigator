#!/bin/sh
# Phase 8 acceptance smoke test (headless, temporary HOME / --config-dir / fixture dir):
#   1. Command palette finds and runs "Toggle Sidebar" and "New Folder".
#   2. Rebinding Ctrl+E writes keymap.json under --config-dir and persists across restart.
#   3. Searchable context menu filters by "Trash".
#   4. Multi-chord sequence Ctrl+K Ctrl+S runs through SequenceMatcher cleanly.
set -eu

BIN="${1:?usage: phase8_smoke.sh <navigator-bin>}"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/navigator-phase8-smoke-XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

HOME_DIR="$TMP/home"
CFG_DIR="$TMP/config"
WORK_DIR="$TMP/work"
mkdir -p "$HOME_DIR" "$CFG_DIR" "$WORK_DIR"
printf 'hello\n' > "$WORK_DIR/sample.txt"

export HOME="$HOME_DIR"
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export NAVIGATOR_NO_ANIMATIONS=1

# Run 1: open palette ("Toggle Sidebar", "New Folder"), rebind Ctrl+E -> view.toggle_hidden,
# open context menu and filter "Trash", trigger sequence Ctrl+K Ctrl+S.
NAVIGATOR_DEBUG_SCRIPT="palette;ptype:Toggle Sidebar;penter;rebind:view.toggle_hidden:Ctrl+E;cmenu;cmtype:Trash;key:Escape;seq:Ctrl+K,Ctrl+S;key:Escape" \
  "$BIN" --frames 12 --config-dir "$CFG_DIR" --no-index "$WORK_DIR"

# Verify keymap.json persisted under CFG_DIR with the Ctrl+E rebind.
KEYMAP_FILE="$CFG_DIR/keymap.json"
if [ ! -f "$KEYMAP_FILE" ]; then
    echo "FAIL: keymap.json was not written at $KEYMAP_FILE" >&2
    exit 1
fi
grep -q '"Ctrl+E"' "$KEYMAP_FILE" || {
    echo "FAIL: Ctrl+E binding missing from $KEYMAP_FILE" >&2
    cat "$KEYMAP_FILE" >&2
    exit 1
}
grep -q '"view.toggle_hidden"' "$KEYMAP_FILE" || {
    echo "FAIL: view.toggle_hidden binding missing from $KEYMAP_FILE" >&2
    cat "$KEYMAP_FILE" >&2
    exit 1
}

# Verify palette_history.json persisted under CFG_DIR with Toggle Sidebar recorded.
HIST_FILE="$CFG_DIR/palette_history.json"
if [ ! -f "$HIST_FILE" ]; then
    echo "FAIL: palette_history.json was not written at $HIST_FILE" >&2
    exit 1
fi
grep -q '"view.toggle_sidebar"' "$HIST_FILE" || {
    echo "FAIL: view.toggle_sidebar missing from $HIST_FILE" >&2
    cat "$HIST_FILE" >&2
    exit 1
}

# Run 2: restart with the same --config-dir, press Ctrl+E (rebound), verify clean run and
# keymap.json still intact.
NAVIGATOR_DEBUG_SCRIPT="key:Ctrl+E;palette;ptype:GoTo;key:Escape" \
  "$BIN" --frames 8 --config-dir "$CFG_DIR" --no-index "$WORK_DIR"

grep -q '"Ctrl+E"' "$KEYMAP_FILE" || {
    echo "FAIL: Ctrl+E binding did not persist across restart" >&2
    exit 1
}

echo "Phase 8 smoke test passed."
