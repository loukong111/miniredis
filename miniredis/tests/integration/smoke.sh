#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /path/to/miniredis" >&2
  exit 2
fi

SERVER="$1"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
CURL="${CURL:-curl}"
TMPDIR="$(mktemp -d /tmp/miniredis_it.XXXXXX)"
PID=""

cleanup() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill -INT "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  rm -rf "${TMPDIR}"
}
trap cleanup EXIT

wait_for_auth_server() {
  local port="$1"
  for _ in $(seq 1 100); do
    if "${REDIS_CLI}" -p "${port}" PING 2>/dev/null | grep -q "NOAUTH"; then
      return 0
    fi
    sleep 0.05
  done
  echo "server on port ${port} did not become ready" >&2
  return 1
}

wait_for_plain_server() {
  local port="$1"
  for _ in $(seq 1 100); do
    if "${REDIS_CLI}" -p "${port}" PING 2>/dev/null | grep -q "PONG"; then
      return 0
    fi
    sleep 0.05
  done
  echo "server on port ${port} did not become ready" >&2
  return 1
}

stop_server() {
  if [[ -n "${PID}" ]]; then
    kill -INT "${PID}" 2>/dev/null || true
    wait "${PID}"
    PID=""
  fi
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

run_auth_kv_stats_snapshot_smoke() {
  local port=17666
  local stats_port=18081
  local snapshot="${TMPDIR}/snapshot_auth.dat"
  local log="${TMPDIR}/auth.log"

  "${SERVER}" \
    --bind 127.0.0.1 \
    --port "${port}" \
    --stats-bind 127.0.0.1 \
    --stats-port "${stats_port}" \
    --snapshot-file "${snapshot}" \
    --snapshot-interval 30 \
    --slowlog-log-slower-than-us 1 \
    --requirepass secret >"${log}" 2>&1 &
  PID="$!"
  wait_for_auth_server "${port}"

  assert_eq "$("${REDIS_CLI}" -p "${port}" PING)" "ERR NOAUTH Authentication required" "unauthenticated ping"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw SET alpha one 2>/dev/null)" "OK" "set alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw SET spaced "hello world" 2>/dev/null)" "OK" "set spaced"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw GET alpha 2>/dev/null)" "one" "get alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw MGET alpha missing spaced 2>/dev/null)" $'one\n\nhello world' "mget mixed"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw TTL alpha 2>/dev/null)" "-1" "ttl persistent alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw EXPIRE alpha 10 2>/dev/null)" "1" "expire alpha"
  local alpha_ttl
  alpha_ttl="$("${REDIS_CLI}" -p "${port}" -a secret --raw TTL alpha 2>/dev/null)"
  if [[ "${alpha_ttl}" -lt 1 || "${alpha_ttl}" -gt 10 ]]; then
    echo "assertion failed for alpha ttl: expected 1..10, got '${alpha_ttl}'" >&2
    exit 1
  fi
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw SET transient value EX 10 2>/dev/null)" "OK" "set transient ex"
  local transient_ttl
  transient_ttl="$("${REDIS_CLI}" -p "${port}" -a secret --raw TTL transient 2>/dev/null)"
  if [[ "${transient_ttl}" -lt 1 || "${transient_ttl}" -gt 10 ]]; then
    echo "assertion failed for transient ttl: expected 1..10, got '${transient_ttl}'" >&2
    exit 1
  fi
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw DEL transient 2>/dev/null)" "1" "del transient"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw EXISTS alpha 2>/dev/null)" "1" "exists alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw DEL alpha 2>/dev/null)" "1" "del alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw EXISTS alpha 2>/dev/null)" "0" "exists deleted alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw COMMAND COUNT 2>/dev/null)" "34" "command count"
  "${REDIS_CLI}" -p "${port}" -a secret --raw COMMAND INFO GET SET missing 2>/dev/null | grep -q '^get$'
  local info_memory
  info_memory="$("${REDIS_CLI}" -p "${port}" -a secret --raw INFO memory 2>/dev/null)"
  grep -q '# Memory' <<<"${info_memory}"
  grep -q 'used_memory:' <<<"${info_memory}"
  grep -q 'maxmemory_policy:' <<<"${info_memory}"
  grep -q 'cache_shards:' <<<"${info_memory}"
  "${REDIS_CLI}" -p "${port}" -a secret --raw INFO server 2>/dev/null | grep -q 'io_threads:'
  local slowlog_len slowlog_get
  slowlog_len="$("${REDIS_CLI}" -p "${port}" -a secret --raw SLOWLOG LEN 2>/dev/null)"
  if [[ "${slowlog_len}" -lt 1 ]]; then
    echo "assertion failed for slowlog len: expected >=1, got '${slowlog_len}'" >&2
    exit 1
  fi
  slowlog_get="$("${REDIS_CLI}" -p "${port}" -a secret --raw SLOWLOG GET 1 2>/dev/null)"
  grep -Eq 'AUTH|PING|SET|GET|MGET|INFO|SLOWLOG|COMMAND' <<<"${slowlog_get}"

  local stats metrics
  stats="$("${CURL}" -fsS "http://127.0.0.1:${stats_port}/stats")"
  grep -q '"key_count":1' <<<"${stats}"
  grep -q '"cache_shards":' <<<"${stats}"
  grep -q '"io_threads":' <<<"${stats}"
  grep -q '"mem_pool_used_blocks":1' <<<"${stats}"
  grep -q '"connected_clients":0' <<<"${stats}"
  grep -q '"total_connections":' <<<"${stats}"
  grep -q '"rejected_connections":0' <<<"${stats}"
  grep -q '"hit_rate":' <<<"${stats}"
  grep -q '"used_memory_bytes":' <<<"${stats}"
  grep -q '"maxmemory_bytes":' <<<"${stats}"
  grep -q '"evicted_keys":' <<<"${stats}"
  grep -q '"latency_samples":' <<<"${stats}"
  grep -q '"avg_command_latency_us":' <<<"${stats}"
  grep -q '"max_command_latency_us":' <<<"${stats}"
  grep -q '"slowlog_len":' <<<"${stats}"
  grep -q '"slowlog_log_slower_than_us":' <<<"${stats}"
  grep -q '"max_request_bytes":' <<<"${stats}"
  grep -q '"max_pipeline_commands":' <<<"${stats}"
  grep -q '"client_output_buffer_limit":' <<<"${stats}"
  grep -q '"snapshot_running":' <<<"${stats}"
  grep -q '"snapshot_last_duration_ms":' <<<"${stats}"
  grep -q '"aof_rewrite_running":' <<<"${stats}"
  grep -q '"aof_last_rewrite_records":' <<<"${stats}"
  "${CURL}" -fsS "http://127.0.0.1:${stats_port}/healthz" | grep -q '"status":"ok"'
  "${CURL}" -fsS "http://127.0.0.1:${stats_port}/readyz" | grep -q '"status":"ready"'
  metrics="$("${CURL}" -fsS "http://127.0.0.1:${stats_port}/metrics")"
  grep -q 'miniredis_total_commands' <<<"${metrics}"
  grep -q 'miniredis_hit_rate' <<<"${metrics}"
  grep -q 'miniredis_used_memory_bytes' <<<"${metrics}"
  grep -q 'miniredis_evicted_keys' <<<"${metrics}"
  grep -q 'miniredis_slowlog_len' <<<"${metrics}"
  grep -q 'miniredis_max_request_bytes' <<<"${metrics}"
  grep -q 'miniredis_snapshot_running' <<<"${metrics}"
  grep -q 'miniredis_aof_rewrite_running' <<<"${metrics}"
  grep -q 'miniredis_ready 1' <<<"${metrics}"
  grep -q 'miniredis_io_threads' <<<"${metrics}"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw SET ttl_restore keep EX 30 2>/dev/null)" "OK" "set ttl_restore"

  stop_server

  "${SERVER}" \
    --bind 127.0.0.1 \
    --port "${port}" \
    --stats-bind 127.0.0.1 \
    --stats-port "${stats_port}" \
    --snapshot-file "${snapshot}" \
    --snapshot-interval 30 \
    --slowlog-log-slower-than-us 1 \
    --requirepass secret >"${log}.restart" 2>&1 &
  PID="$!"
  wait_for_auth_server "${port}"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw GET spaced 2>/dev/null)" "hello world" "snapshot restore"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw GET ttl_restore 2>/dev/null)" "keep" "snapshot ttl restore value"
  local restored_ttl
  restored_ttl="$("${REDIS_CLI}" -p "${port}" -a secret --raw TTL ttl_restore 2>/dev/null)"
  if [[ "${restored_ttl}" -lt 1 || "${restored_ttl}" -gt 30 ]]; then
    echo "assertion failed for restored ttl: expected 1..30, got '${restored_ttl}'" >&2
    exit 1
  fi
  stop_server
}

run_cluster_smoke() {
  local port=17667
  local stats_port=18082
  local snapshot="${TMPDIR}/snapshot_cluster.dat"
  local log="${TMPDIR}/cluster.log"

  "${SERVER}" \
    --cluster \
    --bind 127.0.0.1 \
    --node-addr "127.0.0.1:${port}" \
    --nodes "127.0.0.1:${port}" \
    --stats-bind 127.0.0.1 \
    --stats-port "${stats_port}" \
    --snapshot-file "${snapshot}" \
    --snapshot-interval 30 >"${log}" 2>&1 &
  PID="$!"
  wait_for_plain_server "${port}"

  local info nodes keyslot slots
  info="$("${REDIS_CLI}" -p "${port}" --raw CLUSTER INFO)"
  grep -q 'cluster_enabled:1' <<<"${info}"
  grep -q 'cluster_known_nodes:1' <<<"${info}"
  grep -q 'cluster_slots_assigned:16384' <<<"${info}"
  grep -q 'cluster_failed_nodes:0' <<<"${info}"
  grep -q 'cluster_suspect_nodes:0' <<<"${info}"
  grep -q 'cluster_current_epoch:' <<<"${info}"

  nodes="$("${REDIS_CLI}" -p "${port}" --raw CLUSTER NODES)"
  grep -q "127.0.0.1:${port}" <<<"${nodes}"
  grep -q 'myself,master' <<<"${nodes}"
  grep -q '0-16383' <<<"${nodes}"

  local myid
  myid="$("${REDIS_CLI}" -p "${port}" --raw CLUSTER MYID)"
  if [[ ! "${myid}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "assertion failed for cluster myid: expected 40 hex chars, got '${myid}'" >&2
    exit 1
  fi

  keyslot="$("${REDIS_CLI}" -p "${port}" --raw CLUSTER KEYSLOT 'foo{bar}1')"
  assert_eq "${keyslot}" "5061" "cluster keyslot"

  slots="$("${REDIS_CLI}" -p "${port}" --raw CLUSTER SLOTS)"
  grep -q '127.0.0.1' <<<"${slots}"
  grep -q "${port}" <<<"${slots}"

  assert_eq "$("${REDIS_CLI}" -p "${port}" --raw SET cluster_probe ok)" "OK" "cluster set"
  assert_eq "$("${REDIS_CLI}" -p "${port}" --raw GET cluster_probe)" "ok" "cluster get"
  assert_eq "$("${REDIS_CLI}" -p "${port}" --raw SET 'foo{bar}1' one)" "OK" "cluster hash tag set"
  assert_eq "$("${REDIS_CLI}" -p "${port}" --raw CLUSTER COUNTKEYSINSLOT "${keyslot}")" "1" "cluster countkeysinslot"
  stop_server
}

run_auth_kv_stats_snapshot_smoke
run_cluster_smoke

echo "integration smoke tests passed"
