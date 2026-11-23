#!/bin/sh
# Phase 6 headless app (owner: qa): the Inspector and the thumbnail rules in the real binary.
#   1. Inspector smoke on a fixture folder (PNG, a video made with the ffmpeg CLI, text, a
#      folder): each one is inspected and reaches "status ready" with its kind.
#   2. Space through the real key path: key:Space shows / hides the Inspector, Alt+KeypadEnter
#      shows it, and Space while the folder filter has focus never toggles it.
#   3. Headless runs without --thumbnail-dir / --cache-dir create nothing under a temporary HOME,
#      XDG_CACHE_HOME or XDG_CONFIG_HOME, and never spawn a thumbnailer: a fake thumbnailer
#      (XDG_DATA_DIRS) and a fake bwrap (first on PATH) would each leave a marker.
#   4. Control: with --thumbnail-dir the same fake thumbnailer IS run (the markers appear, so
#      check 3 is meaningful); with --no-external-thumbnailers it is not.
# Usage: preview_app_smoke.sh <navigator binary> [frames]
# Exit 77 (skipped) without ffmpeg. Everything lives in a mktemp -d directory removed on exit;
# SDL dummy video driver only, never a real display, never the user's ~/.cache.
set -u
bin=$1
frames=${2:-30}
command -v ffmpeg >/dev/null 2>&1 || { echo "SKIP: ffmpeg is not installed"; exit 77; }
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-preview-app-XXXXXX") || exit 1
trap 'rm -rf "$root"' EXIT INT TERM
export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
export HOME="$root/home"
export XDG_CONFIG_HOME="$root/xdg-config"
export XDG_CACHE_HOME="$root/xdg-cache"
export XDG_DATA_HOME="$root/xdg-data"
export XDG_DATA_DIRS="$root/share"
unset NAVIGATOR_DEBUG_VOLUMES NAVIGATOR_DEBUG_INDEX_ROOT DISPLAY WAYLAND_DISPLAY
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$root/share/thumbnailers" \
    "$root/bin" "$root/markers" "$root/out"
fail=0

# ---- fixtures ---------------------------------------------------------------------------------
fx=$root/fx
mkdir -p "$fx/folder"
: >"$fx/folder/a.txt"
: >"$fx/folder/b.txt"
printf 'Navigator\nPhase 6 smoke\n' >"$fx/notes.txt"
ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240 -frames:v 1 "$fx/photo.png" &&
    ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240 -frames:v 1 "$fx/scan.tiff" &&
    ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=10 -t 2 \
        -pix_fmt yuv420p "$fx/clip.mp4" || { echo "FAIL: ffmpeg could not make the fixtures"; exit 1; }

# ---- fake thumbnailer and fake bwrap: each leaves a marker when it runs -----------------------
cat >"$root/bin/navigator-fake-thumbnailer" <<EOF
#!/bin/sh
: >"$root/markers/thumbnailer.\$\$"
exit 1
EOF
cat >"$root/bin/bwrap" <<EOF
#!/bin/sh
: >"$root/markers/bwrap.\$\$"
exit 1
EOF
chmod 755 "$root/bin/navigator-fake-thumbnailer" "$root/bin/bwrap"
cat >"$root/share/thumbnailers/navigator-fake.thumbnailer" <<EOF
[Thumbnailer Entry]
TryExec=navigator-fake-thumbnailer
Exec=navigator-fake-thumbnailer %i %o %s
MimeType=image/tiff;image/png;video/mp4;text/plain;
EOF
export PATH="$root/bin:$PATH"

say_fail() { # message, file to show
    echo "FAIL $1"
    [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/    /' "$2"
    fail=1
}

run() { # name, args...
    name=$1
    shift
    "$bin" --frames "$frames" --no-session --no-index "$@" >"$root/$name.log" 2>&1
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

expect() { # dump file, pattern
    grep -Eq "$2" "$1" || say_fail "$(basename "$1"): expected '$2'" "$1"
}

nothing_created() { # name
    left=$(find "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" -mindepth 1 2>/dev/null)
    [ -z "$left" ] || say_fail "$1: files created under HOME / XDG dirs: $left"
    spawned=$(ls "$root/markers")
    [ -z "$spawned" ] || say_fail "$1: a thumbnailer or bwrap was spawned: $spawned"
}

o=$root/out
# ---- 1. Inspector smoke, headless without dirs (also check 3) ---------------------------------
if run inspect --debug-script "inspect:photo.png;waitpreview;dump:$o/png;inspect:clip.mp4;waitpreview;dump:$o/video;inspect:notes.txt;waitpreview;dump:$o/text;inspect:folder;waitpreview;dump:$o/folder;inspect:scan.tiff;waitpreview;dump:$o/tiff;waitthumbs" "$fx"; then
    expect "$o/png" "^inspector visible .* status ready kind image item photo.png"
    expect "$o/video" "^inspector visible .* status ready kind video item clip.mp4"
    expect "$o/text" "^inspector visible .* status ready kind text item notes.txt"
    expect "$o/folder" "^inspector visible .* status ready kind folder item folder"
    # No built-in TIFF decoder and no thumbnailers in a headless run: a placeholder, not a crash.
    expect "$o/tiff" "^inspector visible .* status (ready kind placeholder|failed) item scan.tiff"
fi
nothing_created inspect

# ---- 2. Space through the real key path -------------------------------------------------------
if run space --debug-script "click:notes.txt;key:Space;waitpreview;dump:$o/s1;key:Space;dump:$o/s2;key:Alt+KeypadEnter;waitpreview;dump:$o/s3;key:Space;filter:no;key:Space;key:Shift+Space;dump:$o/s4;blurfilter;key:Space;waitpreview;dump:$o/s5" "$fx"; then
    expect "$o/s1" "^inspector visible .* status ready kind text item notes.txt"
    expect "$o/s2" "^inspector hidden"
    expect "$o/s3" "^inspector visible .* item notes.txt"
    expect "$o/s4" "^inspector hidden" # Space and Shift+Space typed into the filter field
    expect "$o/s5" "^inspector visible .* item notes.txt"
fi
nothing_created space

# ---- 3. list thumbnails without dirs: still nothing on disk, nothing spawned ------------------
if run nodirs --debug-script "waitthumbs;cmd:nav.refresh;waitthumbs;dump:$o/nodirs" "$fx"; then
    [ -e "$HOME/.cache/thumbnails" ] && say_fail "nodirs: ~/.cache/thumbnails created"
fi
nothing_created nodirs

# ---- 4. control: an explicit --thumbnail-dir runs the fake thumbnailer ------------------------
if run withdir --thumbnail-dir "$root/thumbs" --no-external-thumbnailers \
    --debug-script "inspect:scan.tiff;waitpreview;waitthumbs;dump:$o/noext" "$fx"; then
    spawned=$(ls "$root/markers")
    [ -z "$spawned" ] || say_fail "--no-external-thumbnailers: spawned $spawned"
fi
if run external --thumbnail-dir "$root/thumbs" \
    --debug-script "inspect:scan.tiff;waitpreview;waitthumbs;dump:$o/ext" "$fx"; then
    ls "$root/markers" | grep -q '^thumbnailer\.' ||
        say_fail "control: the fake thumbnailer never ran (check 3 would prove nothing)" "$root/external.log"
    # The failed fake leaves a failure marker in the explicit cache only.
    find "$root/thumbs" -path '*/fail/navigator-1/*.png' | grep -q . ||
        say_fail "control: no failure marker in --thumbnail-dir"
fi
left=$(find "$HOME" "$XDG_CACHE_HOME" -mindepth 1 2>/dev/null)
[ -z "$left" ] || say_fail "control: files created under HOME / XDG_CACHE_HOME: $left"

[ "$fail" -eq 0 ] && echo "OK: Inspector image/video/text/folder, Space routing, no cache writes or thumbnailer spawns without dirs"
exit "$fail"
