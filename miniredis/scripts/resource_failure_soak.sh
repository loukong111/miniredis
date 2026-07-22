#!/usr/bin/env bash
set -euo pipefail

SERVER="${SERVER:-build/miniredis}"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
CURL="${CURL:-curl}"
REDIS_BENCHMARK="${REDIS_BENCHMARK:-redis-benchmark}"
PORT="${PORT:-19866}"
STATS_PORT="${STATS_PORT:-19086}"
PASSWORD="${PASSWORD:-secret}"
REQUESTS="${REQUESTS:-5000}"
CLIENTS="${CLIENTS:-10}"
TMPDIR="$(mktemp -d /tmp/miniredis_resource.XXXXXX)"
PID=""

SNAPSHOT="${TMPDIR}/snapshot.dat"
AOF="${TMPDIR}/appendonly.aof"
LOG_FILE="${TMPDIR}/miniredis.log"

cleanup() {
  stop_server
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

start_server() {
  local maxmemory="$1"
  local eviction_policy="$2"

  stop_server
  "${SERVER}" \
    --bind 127.0.0.1 \
    --port "${PORT}" \
    --stats-bind 127.0.0.1 \
    --stats-port "${STATS_PORT}" \
    --snapshot-file "${SNAPSHOT}" \
    --snapshot-interval 1 \
    --appendonly-file "${AOF}" \
    --appendfsync always \
    --requirepass "${PASSWORD}" \
    --maxmemory "${maxmemory}" \
    --eviction-policy "${eviction_policy}" \
    --cache-shards 1 \
    --io-threads 2 \
    --slowlog-log-slower-than-us 1 \
    --log-file "${LOG_FILE}" \
    --log-level info &
  PID="$!"
  wait_ready
}

wait_ready() {
  for _ in $(seq 1 100); do
    if "${REDIS_CLI}" -p "${PORT}" PING 2>/dev/null | grep -q "NOAUTH"; then
      return 0
    fi
    sleep 0.05
  done
  echo "server did not become ready on port ${PORT}" >&2
  if [[ -f "${LOG_FILE}" ]]; then
    sed -n '1,160p' "${LOG_FILE}" >&2
  fi
  exit 1
}

auth_cli() {
  "${REDIS_CLI}" -p "${PORT}" -a "${PASSWORD}" --raw "$@" 2>/dev/null
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

assert_gt_zero() {
  local value="$1"
  local label="$2"
  if [[ ! "${value}" =~ ^[0-9]+$ || "${value}" -le 0 ]]; then
    echo "assertion failed for ${label}: expected > 0, got '${value}'" >&2
    exit 1
  fi
}

info_value() {
  local section="$1"
  local key="$2"
  auth_cli INFO "${section}" | awk -F: -v k="${key}" '$1 == k { gsub(/\r/, "", $2); print $2; exit }'
}

wait_rewrite_done() {
  for _ in $(seq 1 100); do
    if [[ "$(info_value persistence aof_rewrite_running)" == "0" ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "AOF rewrite did not finish" >&2
  auth_cli INFO persistence >&2 || true
  exit 1
}

echo "MiniRedis resource/failure soak"
echo "workdir: ${TMPDIR}"
require_tool "${REDIS_CLI}"
require_tool "${CURL}"

echo "[1/6] maxmemory noeviction rejects oversized writes"
start_server 512 noeviction
big_value="$(python3 - <<'PY'
print("x" * 2048)
PY
)"
oom_result="$(auth_cli SET too_big "${big_value}" || true)"
if [[ "${oom_result}" != *"OOM maxmemory limit reached"* ]]; then
  echo "assertion failed for noeviction OOM: got '${oom_result}'" >&2
  exit 1
fi
stop_server
rm -f "${SNAPSHOT}" "${SNAPSHOT}.bak" "${SNAPSHOT}.bad" "${AOF}" "${AOF}.rewrite.tmp"

echo "[2/6] maxmemory lru evicts under write pressure"
start_server 4096 lru
value_128="$(python3 - <<'PY'
print("v" * 128)
PY
)"
for i in $(seq 1 120); do
  auth_cli SET "lru:${i}" "${value_128}" >/dev/null
done
evicted="$(info_value memory evicted_keys)"
assert_gt_zero "${evicted}" "evicted_keys"
used_memory="$(info_value memory used_memory)"
maxmemory="$(info_value memory maxmemory)"
if [[ "${used_memory}" -gt "${maxmemory}" ]]; then
  echo "assertion failed for used_memory <= maxmemory: ${used_memory} > ${maxmemory}" >&2
  exit 1
fi

echo "[3/6] AOF rewrite completes and compact log survives restart"
assert_eq "$(auth_cli SET stable before)" "OK" "set stable before rewrite"
assert_eq "$(auth_cli SET rewrite:ttl alive EX 60)" "OK" "set rewrite ttl"
assert_eq "$(auth_cli BGREWRITEAOF)" "Background append only file rewriting started" "bgrewriteaof"
wait_rewrite_done
rewrite_status="$(info_value persistence aof_rewrite_last_status)"
assert_eq "${rewrite_status}" "ok" "rewrite status"
kill_server
start_server 65536 lru
assert_eq "$(auth_cli GET stable)" "before" "stable after rewrite restart"
assert_eq "$(auth_cli GET rewrite:ttl)" "alive" "ttl after rewrite restart"

echo "[4/6] incomplete AOF tail is ignored on restart"
stop_server
printf '*3\r\n$3\r\nSET\r\n$6\r\nbroken\r\n$' >>"${AOF}"
start_server 65536 lru
assert_eq "$(auth_cli GET stable)" "before" "stable after bad AOF tail"
assert_eq "$(auth_cli EXISTS broken)" "0" "bad tail ignored"

echo "[5/6] leftover rewrite temp file is removed without replacing old AOF"
stop_server
printf '*3\r\n$3\r\nSET\r\n$6\r\nstable\r\n$9\r\nfrom-temp\r\n' >"${AOF}.rewrite.tmp"
start_server 65536 lru
assert_eq "$(auth_cli GET stable)" "before" "stable ignores temp rewrite"
if [[ -f "${AOF}.rewrite.tmp" ]]; then
  echo "assertion failed: leftover rewrite temp still exists" >&2
  exit 1
fi

echo "[6/6] short concurrent pressure and observability check"
if command -v "${REDIS_BENCHMARK}" >/dev/null 2>&1; then
  "${REDIS_BENCHMARK}" -p "${PORT}" -a "${PASSWORD}" \
    -n "${REQUESTS}" -c "${CLIENTS}" -d 128 -t set,get >"${TMPDIR}/benchmark.txt" 2>&1
else
  for i in $(seq 1 500); do
    auth_cli SET "fallback:${i}" "${i}" >/dev/null
    assert_eq "$(auth_cli GET "fallback:${i}")" "${i}" "fallback get ${i}"
  done
fi

"${CURL}" -fsS "http://127.0.0.1:${STATS_PORT}/healthz" | grep -q '"status":"ok"'
"${CURL}" -fsS "http://127.0.0.1:${STATS_PORT}/readyz" | grep -q '"status":"ready"'
stats="$("${CURL}" -fsS "http://127.0.0.1:${STATS_PORT}/stats")"
grep -q '"evicted_keys":' <<<"${stats}"
grep -q '"aof_rewrite_running":' <<<"${stats}"
grep -Eq '"aof_rewrite_last_status":"(ok|cleaned_tmp)"' <<<"${stats}"
metrics="$("${CURL}" -fsS "http://127.0.0.1:${STATS_PORT}/metrics")"
grep -q 'miniredis_evicted_keys' <<<"${metrics}"
grep -q 'miniredis_aof_rewrite_last_status_info' <<<"${metrics}"

stop_server
echo "resource/failure soak passed"
