#!/usr/bin/env bash
# Builds the Navigator .deb package using the CMake Release (LTO) preset and CPack.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/release-deb"
OUT_DIR="${ROOT_DIR}/dist"

cmake --preset release -B "${BUILD_DIR}" \
    -DNAVIGATOR_BUILD_TESTS=OFF \
    -DNAVIGATOR_WERROR=OFF

cmake --build "${BUILD_DIR}" --parallel

mkdir -p "${OUT_DIR}"
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G DEB -B "${OUT_DIR}"

(
    cd "${OUT_DIR}"
    sha256sum navigator_*.deb > SHA256SUMS.txt
)

echo "Built .deb in ${OUT_DIR}:"
ls -lh "${OUT_DIR}"/navigator_*.deb "${OUT_DIR}/SHA256SUMS.txt"
