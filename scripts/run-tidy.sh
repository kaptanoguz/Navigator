#!/usr/bin/env bash
# Navigator clang-tidy gate.
#   scripts/run-tidy.sh [build-dir]   (default: build/dev; needs compile_commands.json)
# Checks project .cpp files under src/ and tests/ (never _deps). Exits non-zero on any finding.
# Overrides: CLANG_TIDY=/path/to/clang-tidy, RUN_CLANG_TIDY=/path/to/run-clang-tidy, JOBS=N.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

build_dir="${1:-build/dev}"
build_dir="$(cd "$build_dir" 2>/dev/null && pwd)" || {
    echo "error: build dir '${1:-build/dev}' not found; run 'cmake --preset dev' first" >&2
    exit 2
}
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo "error: $build_dir/compile_commands.json missing (CMAKE_EXPORT_COMPILE_COMMANDS=ON)" >&2
    exit 2
fi

first_found() {
    local c
    for c in "$@"; do
        if [[ -n "$c" ]] && command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    return 1
}

if [[ -n "${CLANG_TIDY:-}" ]]; then
    tidy="$(first_found "$CLANG_TIDY")" || { echo "error: '$CLANG_TIDY' not found" >&2; exit 2; }
else
    tidy="$(first_found clang-tidy-18 clang-tidy)" || { echo "error: clang-tidy not found" >&2; exit 2; }
fi
if [[ -n "${RUN_CLANG_TIDY:-}" ]]; then
    runner="$(first_found "$RUN_CLANG_TIDY")" || runner=""
else
    runner="$(first_found run-clang-tidy-18 run-clang-tidy)" || runner=""
fi
jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# Project .cpp files that actually appear in the compilation database.
mapfile -t files < <(
    python3 -I - "$build_dir/compile_commands.json" "$root" <<'PY'
import json, os, sys
db, root = sys.argv[1], os.path.realpath(sys.argv[2])
seen = set()
for e in json.load(open(db)):
    f = os.path.realpath(os.path.join(e.get("directory", ""), e["file"]))
    rel = os.path.relpath(f, root)
    if rel.startswith(".."):
        continue
    parts = rel.split(os.sep)
    if parts[0] not in ("src", "tests") or "_deps" in parts or parts[0].startswith("build"):
        continue
    if not f.endswith((".cpp", ".cc", ".cxx")):
        continue
    if f not in seen:
        seen.add(f)
        print(f)
PY
)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "run-tidy: no project sources in $build_dir/compile_commands.json"
    exit 0
fi

echo "run-tidy: $("$tidy" --version | grep -m1 -i version) on ${#files[@]} files"

log="$(mktemp)"
trap 'rm -f "$log"' EXIT
status=0
if [[ -n "$runner" ]]; then
    # run-clang-tidy takes regexes; anchor each file exactly.
    regexes=()
    for f in "${files[@]}"; do
        regexes+=("^$(printf '%s' "$f" | sed 's/[][\.*^$+?(){}|/]/\\&/g')\$")
    done
    "$runner" -quiet -clang-tidy-binary "$tidy" -p "$build_dir" -j "$jobs" "${regexes[@]}" \
        2>&1 | tee "$log" || status=$?
else
    for f in "${files[@]}"; do
        "$tidy" --quiet -p "$build_dir" "$f" 2>&1 | tee -a "$log" || status=1
    done
fi

# WarningsAsErrors makes findings errors, but belt-and-braces: any diagnostic fails the gate.
if grep -Eq '(warning|error): ' "$log"; then
    status=1
fi
if [[ $status -ne 0 ]]; then
    echo "run-tidy: FAILED" >&2
    exit 1
fi
echo "run-tidy: OK"
