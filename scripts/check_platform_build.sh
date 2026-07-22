#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BUILD_DIR="${SERVER_BUILD_DIR:-/tmp/miniredis-platform-server}"
CLIENT_BUILD_DIR="${CLIENT_BUILD_DIR:-/tmp/miniredis-platform-client}"

cmake -S "${ROOT_DIR}" -B "${SERVER_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINIREDIS_BUILD_SERVER=ON \
  -DMINIREDIS_BUILD_TESTS=ON \
  -DMINIREDIS_BUILD_QT_CONSOLE=OFF \
  -DMINIREDIS_ENABLE_INTEGRATION_TESTS=OFF

cmake --build "${SERVER_BUILD_DIR}" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
ctest --test-dir "${SERVER_BUILD_DIR}" -R miniredis_unit_tests --output-on-failure

cmake -S "${ROOT_DIR}" -B "${CLIENT_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINIREDIS_BUILD_SERVER=OFF \
  -DMINIREDIS_BUILD_TESTS=OFF \
  -DMINIREDIS_BUILD_QT_CONSOLE="${MINIREDIS_BUILD_QT_CONSOLE:-OFF}"

echo "Platform build boundary check passed."
