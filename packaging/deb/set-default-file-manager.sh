#!/usr/bin/env bash
# Sets Navigator as the default handler for inode/directory (folders) after user confirmation.
set -euo pipefail

if ! command -v xdg-mime >/dev/null 2>&1; then
    echo "Error: xdg-mime not found (install xdg-utils)." >&2
    exit 1
fi

CURRENT="$(xdg-mime query default inode/directory 2>/dev/null || echo 'none')"
echo "Current default file manager (inode/directory): ${CURRENT}"
echo "This will run: xdg-mime default navigator.desktop inode/directory"

if [ "${1:-}" != "--yes" ]; then
    read -r -p "Set Navigator as default file manager? [y/N] " REPLY
    case "${REPLY}" in
        [yY]|[yY][eE][sS]) ;;
        *)
            echo "Aborted. No changes made."
            exit 0
            ;;
    esac
fi

xdg-mime default navigator.desktop inode/directory
echo "Updated default inode/directory handler to: $(xdg-mime query default inode/directory)"
