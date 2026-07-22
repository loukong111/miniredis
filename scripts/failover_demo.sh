#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${SERVER:-${ROOT_DIR}/build/miniredis}"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
WORKDIR="${WORKDIR:-${ROOT_DIR}/build/failover-demo}"
HOST="${HOST:-127.0.0.1}"
MASTER_PORT="${MASTER_PORT:-6386}"
REPLICA1_PORT="${REPLICA1_PORT:-6387}"
REPLICA2_PORT="${REPLICA2_PORT:-6388}"
PASSWORD="${PASSWORD:-failover-secret}"

MASTER="${HOST}:${MASTER_PORT}"
REPLICA1="${HOST}:${REPLICA1_PORT}"
REPLICA2="${HOST}:${REPLICA2_PORT}"
GROUP="${MASTER},${REPLICA1},${REPLICA2}"

mkdir -p "${WORKDIR}"

pid_file() {
  printf '%s/%s.pid' "${WORKDIR}" "$1"
}

is_running() {
  local file="$1"
  [[ -f "${file}" ]] && kill -0 "$(cat "${file}")" 2>/dev/null
}

wait_ready() {
  local port="$1"
  for _ in $(seq 1 80); do
    if "${REDIS_CLI}" -h "${HOST}" -p "${port}" -a "${PASSWORD}" --no-auth-warning PING \
        2>/dev/null | grep -q '^PONG$'; then
      return 0
    fi
    sleep 0.1
  done
  echo "node ${HOST}:${port} did not become ready" >&2
  return 1
}

start_node() {
  local name="$1"
  local port="$2"
  local stats_port="$3"
  local replicaof="${4:-}"
  local file
  file="$(pid_file "${name}")"
  if is_running "${file}"; then
    return 0
  fi

  local args=(
    --bind "${HOST}"
    --port "${port}"
    --stats-bind "${HOST}"
    --stats-port "${stats_port}"
    --node-addr "${HOST}:${port}"
    --snapshot-file "${WORKDIR}/${name}.dat"
    --snapshot-interval 3600
    --requirepass "${PASSWORD}"
    --automatic-failover
    --failover-nodes "${GROUP}"
    --failover-state-file "${WORKDIR}/${name}.failover.state"
    --failover-heartbeat-interval-ms 200
    --failover-fail-threshold 2
    --replication-sync-interval-ms 200
    --replication-reconnect-delay-ms 100
  )
  if [[ -n "${replicaof}" ]]; then
    args+=(--replicaof "${replicaof}")
  else
    args+=(--replicas "${REPLICA1},${REPLICA2}")
  fi

  "${SERVER}" "${args[@]}" >"${WORKDIR}/${name}.log" 2>&1 &
  echo "$!" >"${file}"
  wait_ready "${port}"
  echo "started ${name} ${HOST}:${port} pid=$(cat "${file}")"
}

stop_node() {
  local name="$1"
  local file
  file="$(pid_file "${name}")"
  if ! is_running "${file}"; then
    rm -f "${file}"
    return 0
  fi
  local pid
  pid="$(cat "${file}")"
  kill "${pid}" 2>/dev/null || true
  for _ in $(seq 1 50); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      rm -f "${file}"
      echo "stopped ${name} pid=${pid}"
      return 0
    fi
    sleep 0.1
  done
  kill -9 "${pid}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null || true
  rm -f "${file}"
}

stop_all() {
  stop_node replica2
  stop_node replica1
  stop_node master
}

clean_state() {
  stop_all >/dev/null 2>&1 || true
  rm -f "${WORKDIR}"/*.dat "${WORKDIR}"/*.bak "${WORKDIR}"/*.tmp \
        "${WORKDIR}"/*.state "${WORKDIR}"/*.log "${WORKDIR}"/*.pid
}

cli() {
  local port="$1"
  shift
  "${REDIS_CLI}" -h "${HOST}" -p "${port}" -a "${PASSWORD}" --no-auth-warning --raw "$@"
}

wait_value() {
  local port="$1"
  local key="$2"
  local expected="$3"
  for _ in $(seq 1 100); do
    if [[ "$(cli "${port}" GET "${key}" 2>/dev/null || true)" == "${expected}" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "${HOST}:${port} did not replicate ${key}=${expected}" >&2
  return 1
}

info_value() {
  local port="$1"
  local field="$2"
  cli "${port}" INFO replication | tr -d '\r' | awk -F: -v key="${field}" '$1 == key {print substr($0, length(key) + 2); exit}'
}

wait_role() {
  local port="$1"
  local role="$2"
  for _ in $(seq 1 120); do
    if [[ "$(info_value "${port}" role 2>/dev/null || true)" == "${role}" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "${HOST}:${port} did not become ${role}" >&2
  return 1
}

smoke() {
  clean_state
  trap stop_all EXIT

  echo "[1/7] starting a three-node failover group"
  start_node master "${MASTER_PORT}" 18086
  start_node replica1 "${REPLICA1_PORT}" 18087 "${MASTER}"
  start_node replica2 "${REPLICA2_PORT}" 18088 "${MASTER}"

  echo "[2/7] waiting for the initial master to obtain quorum"
  for _ in $(seq 1 80); do
    if [[ "$(info_value "${MASTER_PORT}" failover_writes_allowed 2>/dev/null || true)" == "1" ]]; then
      break
    fi
    sleep 0.1
  done
  [[ "$(info_value "${MASTER_PORT}" failover_writes_allowed)" == "1" ]]
  cli "${MASTER_PORT}" SET ha:before master | grep -q '^OK$'
  wait_value "${REPLICA1_PORT}" ha:before master
  wait_value "${REPLICA2_PORT}" ha:before master

  echo "[3/7] stopping the initial master and electing a replacement"
  stop_node master
  wait_role "${REPLICA1_PORT}" master
  wait_role "${REPLICA2_PORT}" slave
  [[ "$(info_value "${REPLICA1_PORT}" failover_leader)" == "${REPLICA1}" ]]
  [[ "$(info_value "${REPLICA2_PORT}" failover_leader)" == "${REPLICA1}" ]]
  local elected_epoch
  elected_epoch="$(info_value "${REPLICA1_PORT}" failover_leader_epoch)"
  echo "election result: leader=${REPLICA1}, epoch=${elected_epoch}, quorum=2/3"

  echo "[4/7] verifying writes and replication after election"
  cli "${REPLICA1_PORT}" SET ha:after elected | grep -q '^OK$'
  wait_value "${REPLICA2_PORT}" ha:after elected
  echo "replication result: key=ha:after, master=${REPLICA1}, replica=${REPLICA2}, value=elected"

  echo "[5/7] restarting the old master as a replica"
  start_node master "${MASTER_PORT}" 18086
  wait_role "${MASTER_PORT}" slave
  for _ in $(seq 1 100); do
    if [[ "$(info_value "${MASTER_PORT}" master_node 2>/dev/null || true)" == "${REPLICA1}" ]]; then
      break
    fi
    sleep 0.1
  done
  [[ "$(info_value "${MASTER_PORT}" master_node)" == "${REPLICA1}" ]]
  wait_value "${MASTER_PORT}" ha:after elected
  echo "recovery result: old-master=${MASTER}, role=slave, follows=${REPLICA1}, data=up-to-date"

  echo "[6/7] verifying stale-master write fencing"
  local readonly_response
  readonly_response="$(cli "${MASTER_PORT}" SET ha:rejected old-master 2>&1 || true)"
  grep -Eq '^(ERR )?READONLY ' <<<"${readonly_response}"
  echo "stale-master fence: ${readonly_response}"

  echo "[7/7] removing quorum and verifying master write fencing"
  stop_node master
  stop_node replica2
  for _ in $(seq 1 100); do
    if [[ "$(info_value "${REPLICA1_PORT}" failover_writes_allowed 2>/dev/null || true)" == "0" ]]; then
      break
    fi
    sleep 0.1
  done
  [[ "$(info_value "${REPLICA1_PORT}" failover_writes_allowed)" == "0" ]]
  local masterdown_response
  masterdown_response="$(cli "${REPLICA1_PORT}" SET ha:rejected no-quorum 2>&1 || true)"
  grep -Eq '^(ERR )?MASTERDOWN ' <<<"${masterdown_response}"
  echo "quorum fence: reachable=1/3, writes_allowed=0, response=${masterdown_response}"

  echo "automatic failover smoke passed: leader=${REPLICA1}, epoch=${elected_epoch}, old master rejoined as replica"
  stop_all
  trap - EXIT
}

case "${1:-smoke}" in
  smoke) smoke ;;
  stop) stop_all ;;
  *) echo "usage: $0 [smoke|stop]" >&2; exit 2 ;;
esac
