#!/usr/bin/env bash
set -euo pipefail

SERVER="${SERVER:-build/miniredis}"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
CURL="${CURL:-curl}"
REDIS_BENCHMARK="${REDIS_BENCHMARK:-redis-benchmark}"
PORT="${PORT:-19766}"
STATS_PORT="${STATS_PORT:-19081}"
PASSWORD="${PASSWORD:-secret}"
REQUESTS="${REQUESTS:-20000}"
CLIENTS="${CLIENTS:-20}"
SOAK_SECONDS="${SOAK_SECONDS:-10}"
TMPDIR="$(mktemp -d /tmp/miniredis_recovery.XXXXXX)"
SNAPSHOT="${TMPDIR}/snapshot.dat"
LOG_FILE="${TMPDIR}/miniredis.log"
PID=""

cleanup() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill -INT "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  if [[ "${KEEP_TMP:-0}" == "1" ]]; then
    echo "kept workdir: ${TMPDIR}"
  else
    rm -rf "${TMPDIR}"
  fi
}
trap cleanup EXIT

require_tool() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "required tool not found: $1" >&2
    exit 2
  fi
}

start_server() {
  "${SERVER}" \
    --bind 127.0.0.1 \
    --port "${PORT}" \
    --stats-bind 127.0.0.1 \
    --stats-port "${STATS_PORT}" \
    --snapshot-file "${SNAPSHOT}" \
    --snapshot-interval 1 \
    --requirepass "${PASSWORD}" \
    --maxmemory 10485760 \
    --eviction-policy lru \
    --slowlog-log-slower-than-us 1 \
    --log-file "${LOG_FILE}" \
    --log-level info &
  PID="$!"
}

wait_ready() {
  for _ in $(seq 1 100); do
    if "${REDIS_CLI}" -p "${PORT}" PING 2>/dev/null | grep -q "NOAUTH"; then
      return 0
    fi
    sleep 0.05
  done
  echo "server did not become ready on port ${PORT}" >&2
  exit 1
}

stop_server() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill -INT "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  PID=""
}

kill_server() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill -9 "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  PID=""
}

assert_eq() {
  local actual="$1"
  local expected="$2"
  local label="$3"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "assertion failed for ${label}: expected '${expected}', got '${actual}'" >&2
    exit 1
  fi
}

auth_cli() {
  "${REDIS_CLI}" -p "${PORT}" -a "${PASSWORD}" --raw "$@" 2>/dev/null
}

echo "MiniRedis recovery/soak test"
echo "workdir: ${TMPDIR}"
require_tool "${REDIS_CLI}"
require_tool "${CURL}"

echo "[1/5] start server and write snapshot data"
start_server
wait_ready
assert_eq "$(auth_cli SET durable value)" "OK" "set durable"
assert_eq "$(auth_cli SET ttl_key alive EX 60)" "OK" "set ttl_key"
sleep 2
test -f "${SNAPSHOT}"

echo "[2/5] kill -9 and verify snapshot recovery"
kill_server
start_server
wait_ready
assert_eq "$(auth_cli GET durable)" "value" "durable after kill"
assert_eq "$(auth_cli GET ttl_key)" "alive" "ttl_key after kill"
ttl="$(auth_cli TTL ttl_key)"
if [[ "${ttl}" -lt 1 || "${ttl}" -gt 60 ]]; then
  echo "assertion failed for ttl after kill: expected 1..60, got '${ttl}'" >&2
  exit 1
fi

echo "[3/5] verify .bak fallback when main snapshot is corrupted"
assert_eq "$(auth_cli SET newer value2)" "OK" "set newer"
sleep 2
test -f "${SNAPSHOT}.bak"
stop_server
{
  printf 'MINIREDIS_SNAPSHOT_V1\n'
  python3 - <<'PY'
import sys
sys.stdout.buffer.write((1000001).to_bytes(8, "little"))
PY
} >"${SNAPSHOT}"
start_server
wait_ready
test -f "${SNAPSHOT}.bad"
assert_eq "$(auth_cli GET durable)" "value" "durable from backup"

echo "[4/5] run short benchmark/soak"
if command -v "${REDIS_BENCHMARK}" >/dev/null 2>&1; then
  "${REDIS_BENCHMARK}" -p "${PORT}" -a "${PASSWORD}" \
    -n "${REQUESTS}" -c "${CLIENTS}" -d 128 -t set,get >"${TMPDIR}/benchmark.txt" 2>&1
else
  end=$((SECONDS + SOAK_SECONDS))
  i=0
  while [[ "${SECONDS}" -lt "${end}" ]]; do
    auth_cli SET "soak:${i}" "${i}" >/dev/null
    assert_eq "$(auth_cli GET "soak:${i}")" "${i}" "soak get ${i}"
    i=$((i + 1))
  done
fi

echo "[5/5] verify observability endpoints"
"${CURL}" -fsS "http://127.0.0.1:${STATS_PORT}/healthz" | grep -q '"status":"ok"'
stats="$("${CURL}" -fsS "http://127.0.0.1:${STATS_PORT}/stats")"
grep -q '"total_commands":' <<<"${stats}"
grep -q '"slowlog_len":' <<<"${stats}"
metrics="$("${CURL}" -fsS "http://127.0.0.1:${STATS_PORT}/metrics")"
grep -q 'miniredis_total_commands' <<<"${metrics}"
grep -q 'miniredis_slowlog_len' <<<"${metrics}"
grep -q 'snapshot saved successfully' "${LOG_FILE}"

stop_server
echo "recovery/soak test passed"
