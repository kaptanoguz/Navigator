#!/usr/bin/env bash
# Navigator format gate.
#   scripts/check-format.sh          dry-run, exits non-zero on any violation
#   scripts/check-format.sh --fix    rewrite files in place
# Override the binary with CLANG_FORMAT=/path/to/clang-format.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

mode="check"
case "${1:-}" in
"") ;;
--fix) mode="fix" ;;
-h | --help)
    sed -n '2,5p' "$0"
    exit 0
    ;;
*)
    echo "usage: $0 [--fix]" >&2
    exit 2
    ;;
esac

find_tool() {
    local override="$1"
    shift
    if [[ -n "$override" ]]; then
        command -v "$override" || { echo "error: '$override' not found" >&2; return 1; }
        return 0
    fi
    local c
    for c in "$@"; do
        if command -v "$c" >/dev/null 2>&1; then
            command -v "$c"
            return 0
        fi
    done
    echo "error: none of [$*] found; set the env override" >&2
    return 1
}

cf="$(find_tool "${CLANG_FORMAT:-}" clang-format-18 clang-format)"

# Tracked + untracked (not ignored) C/C++ sources under include/ src/ tests/.
list_files() {
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git ls-files -z --cached --others --exclude-standard -- include src tests
    else
        find include src tests -type f -print0 2>/dev/null
    fi
}

files=()
while IFS= read -r -d '' f; do
    case "$f" in
    build*/* | */build*/* | *_deps/*) continue ;;
    esac
    case "$f" in
    *.c | *.h | *.cc | *.cpp | *.cxx | *.hh | *.hpp | *.hxx | *.inl | *.ipp) ;;
    *) continue ;;
    esac
    [[ -f "$f" ]] && files+=("$f")
done < <(list_files)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "check-format: no C/C++ files found"
    exit 0
fi

echo "check-format: $("$cf" --version | head -n1) on ${#files[@]} files ($mode)"
if [[ "$mode" == "fix" ]]; then
    "$cf" -i --style=file "${files[@]}"
    exit 0
fi

if "$cf" --dry-run -Werror --style=file "${files[@]}"; then
    echo "check-format: OK"
else
    echo "check-format: violations found; run scripts/check-format.sh --fix" >&2
    exit 1
fi
