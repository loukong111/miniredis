#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${MINIREDIS_BINARY:-}"

if [[ -z "${BINARY}" ]]; then
  if [[ -x "${ROOT_DIR}/bin/miniredis" ]]; then
    BINARY="${ROOT_DIR}/bin/miniredis"
  else
    BINARY="${ROOT_DIR}/build/miniredis"
  fi
fi

if [[ "${EUID}" -ne 0 ]]; then
  echo "run as root, for example: sudo deploy/install.sh" >&2
  exit 1
fi

if [[ ! -x "${BINARY}" ]]; then
  echo "missing ${BINARY}; run scripts/package_release.sh or build with cmake first" >&2
  exit 1
fi

if ! id -u miniredis >/dev/null 2>&1; then
  useradd --system --no-create-home --shell /usr/sbin/nologin miniredis
fi

install -d -m 0755 /opt/miniredis
install -d -m 0750 -o miniredis -g miniredis /var/lib/miniredis /var/log/miniredis
install -d -m 0750 /etc/miniredis

install -m 0755 "${BINARY}" /opt/miniredis/miniredis
install -m 0644 "${ROOT_DIR}/config/miniredis.prod.conf" /etc/miniredis/miniredis.conf
install -m 0644 "${ROOT_DIR}/deploy/miniredis.service" /etc/systemd/system/miniredis.service
install -m 0644 "${ROOT_DIR}/deploy/logrotate/miniredis" /etc/logrotate.d/miniredis

if [[ ! -f /etc/miniredis/miniredis.env ]]; then
  install -m 0600 "${ROOT_DIR}/deploy/miniredis.env.example" /etc/miniredis/miniredis.env
  echo "created /etc/miniredis/miniredis.env; edit MINIREDIS_REQUIREPASS before exposing the service"
fi

chown miniredis:miniredis /var/lib/miniredis /var/log/miniredis
systemctl daemon-reload

echo "installed MiniRedis. Start with: systemctl enable --now miniredis"
