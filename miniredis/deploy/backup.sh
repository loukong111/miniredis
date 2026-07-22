#!/usr/bin/env bash
set -euo pipefail

BACKUP_ROOT="${BACKUP_ROOT:-/var/backups/miniredis}"
DATA_DIR="${DATA_DIR:-/var/lib/miniredis}"
CONFIG_DIR="${CONFIG_DIR:-/etc/miniredis}"
OPT_DIR="${OPT_DIR:-/opt/miniredis}"
SERVICE_NAME="${SERVICE_NAME:-miniredis}"
TIMESTAMP="$(date +%Y%m%d%H%M%S)"
OUT="${OUT:-${BACKUP_ROOT}/miniredis-backup-${TIMESTAMP}.tar.gz}"
MANIFEST="$(mktemp /tmp/miniredis_backup_manifest.XXXXXX)"

cleanup() {
  rm -f "${MANIFEST}"
}
trap cleanup EXIT

add_path() {
  local path="$1"
  if [[ -e "${path}" ]]; then
    printf '%s\n' "${path}" >>"${MANIFEST}"
  else
    echo "skip missing path: ${path}" >&2
  fi
}

mkdir -p "$(dirname "${OUT}")"

if command -v systemctl >/dev/null 2>&1; then
  systemctl is-active --quiet "${SERVICE_NAME}" && echo "service ${SERVICE_NAME} is running; backup will be crash-consistent"
fi

add_path "${DATA_DIR}"
add_path "${CONFIG_DIR}"
add_path "${OPT_DIR}/miniredis"
add_path "/etc/systemd/system/${SERVICE_NAME}.service"
add_path "/etc/logrotate.d/miniredis"

if [[ ! -s "${MANIFEST}" ]]; then
  echo "nothing to backup; checked ${DATA_DIR}, ${CONFIG_DIR}, ${OPT_DIR}" >&2
  exit 1
fi

tar --absolute-names --warning=no-file-changed -czf "${OUT}" --files-from "${MANIFEST}"
chmod 0600 "${OUT}"

echo "backup written to ${OUT}"
echo "restore example:"
echo "  sudo systemctl stop ${SERVICE_NAME}"
echo "  sudo tar --absolute-names -xzf ${OUT}"
echo "  sudo chown -R miniredis:miniredis ${DATA_DIR} /var/log/miniredis"
echo "  sudo systemctl start ${SERVICE_NAME}"
