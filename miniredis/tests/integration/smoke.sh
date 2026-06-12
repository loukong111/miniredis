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
    --requirepass secret >"${log}" 2>&1 &
  PID="$!"
  wait_for_auth_server "${port}"

  assert_eq "$("${REDIS_CLI}" -p "${port}" PING)" "ERR NOAUTH Authentication required" "unauthenticated ping"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw SET alpha one 2>/dev/null)" "OK" "set alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw SET spaced "hello world" 2>/dev/null)" "OK" "set spaced"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw GET alpha 2>/dev/null)" "one" "get alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw EXISTS alpha 2>/dev/null)" "1" "exists alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw DEL alpha 2>/dev/null)" "1" "del alpha"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw EXISTS alpha 2>/dev/null)" "0" "exists deleted alpha"

  local stats
  stats="$("${CURL}" -fsS "http://127.0.0.1:${stats_port}/stats")"
  grep -q '"key_count":1' <<<"${stats}"
  grep -q '"mem_pool_used_blocks":1' <<<"${stats}"
  grep -q '"connected_clients":0' <<<"${stats}"
  grep -q '"total_connections":' <<<"${stats}"
  grep -q '"rejected_connections":0' <<<"${stats}"
  grep -q '"latency_samples":' <<<"${stats}"
  grep -q '"avg_command_latency_us":' <<<"${stats}"
  grep -q '"max_command_latency_us":' <<<"${stats}"

  stop_server

  "${SERVER}" \
    --bind 127.0.0.1 \
    --port "${port}" \
    --stats-bind 127.0.0.1 \
    --stats-port "${stats_port}" \
    --snapshot-file "${snapshot}" \
    --snapshot-interval 30 \
    --requirepass secret >"${log}.restart" 2>&1 &
  PID="$!"
  wait_for_auth_server "${port}"
  assert_eq "$("${REDIS_CLI}" -p "${port}" -a secret --raw GET spaced 2>/dev/null)" "hello world" "snapshot restore"
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

  local info nodes keyslot
  info="$("${REDIS_CLI}" -p "${port}" --raw CLUSTER INFO)"
  grep -q 'cluster_enabled:1' <<<"${info}"
  grep -q 'cluster_known_nodes:1' <<<"${info}"

  nodes="$("${REDIS_CLI}" -p "${port}" --raw CLUSTER NODES)"
  grep -q "127.0.0.1:${port}" <<<"${nodes}"
  grep -q 'myself,master' <<<"${nodes}"

  keyslot="$("${REDIS_CLI}" -p "${port}" --raw CLUSTER KEYSLOT 'foo{bar}1')"
  assert_eq "${keyslot}" "5061" "cluster keyslot"

  assert_eq "$("${REDIS_CLI}" -p "${port}" --raw SET cluster_probe ok)" "OK" "cluster set"
  assert_eq "$("${REDIS_CLI}" -p "${port}" --raw GET cluster_probe)" "ok" "cluster get"
  stop_server
}

run_auth_kv_stats_snapshot_smoke
run_cluster_smoke

echo "integration smoke tests passed"
