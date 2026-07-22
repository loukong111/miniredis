#!/usr/bin/env bash
set -euo pipefail

SERVER="${SERVER:-build/miniredis}"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
HOST="${HOST:-127.0.0.1}"
MASTER_PORT="${MASTER_PORT:-6366}"
REPLICA_PORT="${REPLICA_PORT:-6367}"
MASTER_STATS_PORT="${MASTER_STATS_PORT:-18080}"
REPLICA_STATS_PORT="${REPLICA_STATS_PORT:-18081}"
WORKDIR="${WORKDIR:-build/replica-demo}"
MASTER_NODE="${HOST}:${MASTER_PORT}"
REPLICA_NODE="${HOST}:${REPLICA_PORT}"

mkdir -p "${WORKDIR}"

usage() {
  cat <<EOF
usage: $0 start|stop|status|smoke|clean

Environment:
  SERVER=${SERVER}
  REDIS_CLI=${REDIS_CLI}
  HOST=${HOST}
  MASTER_PORT=${MASTER_PORT}
  REPLICA_PORT=${REPLICA_PORT}
  WORKDIR=${WORKDIR}
EOF
}

pid_file_for() {
  echo "${WORKDIR}/$1.pid"
}

is_running() {
  local pid_file="$1"
  [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null
}

wait_for_server() {
  local port="$1"
  for _ in $(seq 1 100); do
    if "${REDIS_CLI}" -p "${port}" --raw PING 2>/dev/null | grep -q '^PONG$'; then
      return 0
    fi
    sleep 0.05
  done
  echo "server on port ${port} did not become ready" >&2
  return 1
}

wait_for_value() {
  local port="$1"
  local key="$2"
  local expected="$3"
  for _ in $(seq 1 100); do
    if [[ "$("${REDIS_CLI}" -p "${port}" --raw GET "${key}" 2>/dev/null || true)" == "${expected}" ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "key ${key} on port ${port} did not become ${expected}" >&2
  return 1
}

wait_for_missing_key() {
  local port="$1"
  local key="$2"
  for _ in $(seq 1 100); do
    if [[ "$("${REDIS_CLI}" -p "${port}" --raw EXISTS "${key}" 2>/dev/null || true)" == "0" ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "key ${key} on port ${port} was not deleted" >&2
  return 1
}

start_master() {
  local master_pid
  master_pid="$(pid_file_for master)"

  if ! is_running "${master_pid}"; then
    "${SERVER}" \
      --bind "${HOST}" \
      --port "${MASTER_PORT}" \
      --stats-bind "${HOST}" \
      --stats-port "${MASTER_STATS_PORT}" \
      --snapshot-file "${WORKDIR}/snapshot_master.dat" \
      --replicas "${REPLICA_NODE}" >"${WORKDIR}/master.log" 2>&1 &
    echo "$!" >"${master_pid}"
    echo "started master ${MASTER_NODE} pid=$(cat "${master_pid}")"
  fi
}

start_replica() {
  local replica_pid
  replica_pid="$(pid_file_for replica)"

  if ! is_running "${replica_pid}"; then
    "${SERVER}" \
      --bind "${HOST}" \
      --port "${REPLICA_PORT}" \
      --stats-bind "${HOST}" \
      --stats-port "${REPLICA_STATS_PORT}" \
      --snapshot-file "${WORKDIR}/snapshot_replica.dat" \
      --replicaof "${MASTER_NODE}" >"${WORKDIR}/replica.log" 2>&1 &
    echo "$!" >"${replica_pid}"
    echo "started replica ${REPLICA_NODE} pid=$(cat "${replica_pid}")"
  fi
}

start_pair() {
  start_master
  start_replica
}

stop_replica() {
  local pid_file
  pid_file="$(pid_file_for replica)"
  if is_running "${pid_file}"; then
    local pid
    pid="$(cat "${pid_file}")"
    kill -INT "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  fi
  rm -f "${pid_file}"
}

stop_master() {
  local pid_file
  pid_file="$(pid_file_for master)"
  if is_running "${pid_file}"; then
    local pid
    pid="$(cat "${pid_file}")"
    kill -INT "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  fi
  rm -f "${pid_file}"
}

stop_pair() {
  for name in replica master; do
    local pid_file
    pid_file="$(pid_file_for "${name}")"
    if is_running "${pid_file}"; then
      local pid
      pid="$(cat "${pid_file}")"
      kill -INT "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
      echo "stopped ${name} pid=${pid}"
    fi
    rm -f "${pid_file}"
  done
}

status_pair() {
  for name in master replica; do
    local pid_file
    pid_file="$(pid_file_for "${name}")"
    if is_running "${pid_file}"; then
      echo "${name} running pid=$(cat "${pid_file}")"
    else
      echo "${name} stopped"
    fi
  done
}

smoke_pair() {
  stop_pair >/dev/null 2>&1 || true
  rm -f "${WORKDIR}"/snapshot_*.dat \
        "${WORKDIR}"/snapshot_*.dat.repl.offset \
        "${WORKDIR}"/snapshot_*.dat.repl.state \
        "${WORKDIR}"/snapshot_*.dat.repl.state.tmp
  start_master
  wait_for_server "${MASTER_PORT}"
  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw SET repl:bootstrap before-replica | grep -q '^OK$'
  start_replica
  wait_for_server "${REPLICA_PORT}"

  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw INFO replication | grep -q 'role:master'
  "${REDIS_CLI}" -p "${REPLICA_PORT}" --raw INFO replication | grep -q 'role:slave'
  "${REDIS_CLI}" -p "${REPLICA_PORT}" --raw GET repl:bootstrap | grep -q '^before-replica$'
  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw SET repl:demo one | grep -q '^OK$'
  wait_for_value "${REPLICA_PORT}" repl:demo one
  "${REDIS_CLI}" -p "${REPLICA_PORT}" --raw SET repl:demo blocked | grep -q '^ERR READONLY'
  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw EXPIRE repl:demo 30 | grep -q '^1$'
  local ttl
  for _ in $(seq 1 100); do
    ttl="$("${REDIS_CLI}" -p "${REPLICA_PORT}" --raw TTL repl:demo 2>/dev/null || true)"
    if [[ "${ttl}" =~ ^[0-9]+$ ]] && [[ "${ttl}" -ge 1 ]] && [[ "${ttl}" -le 30 ]]; then
      break
    fi
    sleep 0.05
  done
  if [[ "${ttl}" -lt 1 || "${ttl}" -gt 30 ]]; then
    echo "replica ttl out of range: ${ttl}" >&2
    stop_pair
    exit 1
  fi
  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw DEL repl:demo | grep -q '^1$'
  wait_for_missing_key "${REPLICA_PORT}" repl:demo

  stop_replica
  local start_ns elapsed_ms
  start_ns="$(date +%s%N)"
  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw SET repl:offline catch-up | grep -q '^OK$'
  elapsed_ms=$(( ($(date +%s%N) - start_ns) / 1000000 ))
  if [[ "${elapsed_ms}" -gt 1000 ]]; then
    echo "master write waited ${elapsed_ms}ms for an offline replica" >&2
    stop_pair
    exit 1
  fi
  start_replica
  wait_for_server "${REPLICA_PORT}"
  wait_for_value "${REPLICA_PORT}" repl:offline catch-up

  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw INFO replication | grep -q 'repl_backlog_histlen:'
  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw INFO replication | grep -q 'offset='

  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw SET repl:lifecycle old-master | grep -q '^OK$'
  wait_for_value "${REPLICA_PORT}" repl:lifecycle old-master

  local old_replid old_offset replica_replid
  old_replid="$("${REDIS_CLI}" -p "${MASTER_PORT}" --raw INFO replication |
    awk -F: '$1 == "master_replid" { sub(/\r$/, "", $2); print $2 }')"
  old_offset="$("${REDIS_CLI}" -p "${MASTER_PORT}" --raw INFO replication |
    awk -F: '$1 == "master_repl_offset" { sub(/\r$/, "", $2); print $2 }')"
  replica_replid="$("${REDIS_CLI}" -p "${REPLICA_PORT}" --raw INFO replication |
    awk -F: '$1 == "master_replid" { sub(/\r$/, "", $2); print $2 }')"
  if [[ ! "${old_replid}" =~ ^[0-9a-f]{40}$ ]] ||
     [[ "${replica_replid}" != "${old_replid}" ]] ||
     [[ ! "${old_offset}" =~ ^[0-9]+$ ]] || [[ "${old_offset}" -lt 1 ]]; then
    echo "invalid replication identity before restart" >&2
    stop_pair
    exit 1
  fi

  stop_replica
  stop_master
  start_master
  wait_for_server "${MASTER_PORT}"

  local new_replid new_offset
  new_replid="$("${REDIS_CLI}" -p "${MASTER_PORT}" --raw INFO replication |
    awk -F: '$1 == "master_replid" { sub(/\r$/, "", $2); print $2 }')"
  if [[ ! "${new_replid}" =~ ^[0-9a-f]{40}$ ]] || [[ "${new_replid}" == "${old_replid}" ]]; then
    echo "master replication ID did not change after restart" >&2
    stop_pair
    exit 1
  fi

  local i
  for ((i = 1; i < old_offset; ++i)); do
    "${REDIS_CLI}" -p "${MASTER_PORT}" --raw SET "repl:restart:pad:${i}" "${i}" >/dev/null
  done
  "${REDIS_CLI}" -p "${MASTER_PORT}" --raw SET repl:lifecycle new-master | grep -q '^OK$'
  new_offset="$("${REDIS_CLI}" -p "${MASTER_PORT}" --raw INFO replication |
    awk -F: '$1 == "master_repl_offset" { sub(/\r$/, "", $2); print $2 }')"
  if [[ "${new_offset}" != "${old_offset}" ]]; then
    echo "restart regression setup failed: old offset=${old_offset}, new offset=${new_offset}" >&2
    stop_pair
    exit 1
  fi

  start_replica
  wait_for_server "${REPLICA_PORT}"
  wait_for_value "${REPLICA_PORT}" repl:lifecycle new-master
  wait_for_value "${REPLICA_PORT}" "repl:restart:pad:1" 1
  replica_replid="$("${REDIS_CLI}" -p "${REPLICA_PORT}" --raw INFO replication |
    awk -F: '$1 == "master_replid" { sub(/\r$/, "", $2); print $2 }')"
  if [[ "${replica_replid}" != "${new_replid}" ]]; then
    echo "replica kept stale replication ID after master restart" >&2
    stop_pair
    exit 1
  fi
  grep -q 'master requested full resync' "${WORKDIR}/replica.log"

  echo "replica demo smoke passed (offline write ${elapsed_ms}ms, reconnect catch-up and master lifecycle ID verified)"
  stop_pair
}

case "${1:-}" in
  start)
    start_pair
    ;;
  stop)
    stop_pair
    ;;
  status)
    status_pair
    ;;
  smoke)
    smoke_pair
    ;;
  clean)
    stop_pair
    rm -rf "${WORKDIR}"
    ;;
  *)
    usage
    exit 2
    ;;
esac
