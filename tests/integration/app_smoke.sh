#!/bin/sh
# Headless app smoke (owner: qa): runs the navigator binary on SDL's dummy video driver.
#   1. a fixture folder              -> exits 0 after the frames
#   2. an unreadable start folder    -> exits 0 (empty list + notice, never a crash)
#   3. a start path that does not exist -> exits 0
# Usage: app_smoke.sh <navigator binary> [frames]
# Fixtures live in a mktemp -d directory that is removed on exit. Nothing is activated, so the
# app never launches another program.
set -u
bin=$1
frames=${2:-120}
root=$(mktemp -d "${TMPDIR:-/tmp}/navigator-app-smoke-XXXXXX") || exit 1
cleanup() {
    chmod -R u+rwx "$root" 2>/dev/null
    rm -rf "$root"
}
trap cleanup EXIT INT TERM

export SDL_VIDEODRIVER=dummy
export SDL_RENDER_DRIVER=software
fail=0

run_case() { # name, start path
    log="$root/$1.log"
    "$bin" "$2" --frames "$frames" >"$log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL $1: exit $rc"
        sed 's/^/    /' "$log"
        fail=1
        return
    fi
    if ! grep -q "video driver 'dummy'" "$log"; then
        echo "FAIL $1: no startup log line for the dummy driver"
        sed 's/^/    /' "$log"
        fail=1
        return
    fi
    if grep -Eqi 'AddressSanitizer|ThreadSanitizer|runtime error|LeakSanitizer' "$log"; then
        echo "FAIL $1: sanitizer report"
        sed 's/^/    /' "$log"
        fail=1
        return
    fi
    echo "ok   $1 (exit 0, $frames frames)"
}

mkdir -p "$root/fixture/folder one/inner" "$root/fixture/Ç klasör"
printf 'hello' >"$root/fixture/a.txt"
printf '' >"$root/fixture/.hidden"
i=0
while [ $i -lt 200 ]; do
    : >"$root/fixture/file $i.dat"
    i=$((i + 1))
done
run_case fixture "$root/fixture"

if [ "$(id -u)" -ne 0 ]; then
    mkdir -p "$root/locked"
    : >"$root/locked/secret"
    chmod 000 "$root/locked"
    run_case unreadable "$root/locked"
else
    echo "skip unreadable (running as root)"
fi

run_case missing "$root/does/not/exist"

exit $fail
