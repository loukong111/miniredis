#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-release}"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"
VERSION="${VERSION:-$(date +%Y%m%d%H%M%S)}"
PACKAGE_NAME="${PACKAGE_NAME:-miniredis-linux-x86_64-${VERSION}}"
STAGE_DIR="${DIST_DIR}/${PACKAGE_NAME}"
ARCHIVE="${DIST_DIR}/${PACKAGE_NAME}.tar.gz"

mkdir -p "${DIST_DIR}"
rm -rf "${STAGE_DIR}" "${ARCHIVE}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINIREDIS_BUILD_QT_CONSOLE=OFF \
  -DMINIREDIS_ENABLE_INTEGRATION_TESTS=OFF
cmake --build "${BUILD_DIR}" -j"$(nproc)"

install -d "${STAGE_DIR}/bin" \
          "${STAGE_DIR}/config" \
          "${STAGE_DIR}/deploy/logrotate" \
          "${STAGE_DIR}/docs" \
          "${STAGE_DIR}/scripts"

install -m 0755 "${BUILD_DIR}/miniredis" "${STAGE_DIR}/bin/miniredis"
install -m 0644 "${ROOT_DIR}/config/miniredis.prod.conf" "${STAGE_DIR}/config/miniredis.prod.conf"
install -m 0644 "${ROOT_DIR}/deploy/miniredis.env.example" "${STAGE_DIR}/deploy/miniredis.env.example"
install -m 0644 "${ROOT_DIR}/deploy/miniredis.service" "${STAGE_DIR}/deploy/miniredis.service"
install -m 0755 "${ROOT_DIR}/deploy/install.sh" "${STAGE_DIR}/deploy/install.sh"
install -m 0755 "${ROOT_DIR}/deploy/backup.sh" "${STAGE_DIR}/deploy/backup.sh"
install -m 0644 "${ROOT_DIR}/deploy/logrotate/miniredis" "${STAGE_DIR}/deploy/logrotate/miniredis"
install -m 0755 "${ROOT_DIR}/scripts/recovery_soak.sh" "${STAGE_DIR}/scripts/recovery_soak.sh"
install -m 0755 "${ROOT_DIR}/scripts/resource_failure_soak.sh" "${STAGE_DIR}/scripts/resource_failure_soak.sh"
install -m 0644 "${ROOT_DIR}/README.md" "${STAGE_DIR}/README.md"
install -m 0644 "${ROOT_DIR}/docs/production-deploy.md" "${STAGE_DIR}/docs/production-deploy.md"
install -m 0644 "${ROOT_DIR}/docs/ops-runbook.md" "${STAGE_DIR}/docs/ops-runbook.md"
install -m 0644 "${ROOT_DIR}/docs/usage.md" "${STAGE_DIR}/docs/usage.md"

cat >"${STAGE_DIR}/VERSION" <<EOF
version=${VERSION}
build_time=$(date -Iseconds)
source=${ROOT_DIR}
EOF

tar -C "${DIST_DIR}" -czf "${ARCHIVE}" "${PACKAGE_NAME}"
rm -rf "${STAGE_DIR}"

echo "release package written to ${ARCHIVE}"
