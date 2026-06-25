#!/usr/bin/env bash
set -euo pipefail

SERVER="${SERVER:-build/miniredis}"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
HOST="${HOST:-127.0.0.1}"
PORTS="${PORTS:-6366 6367 6368}"
BASE_STATS_PORT="${BASE_STATS_PORT:-18080}"
WORKDIR="${WORKDIR:-build/cluster-demo}"
NODES=""

mkdir -p "${WORKDIR}"

for port in ${PORTS}; do
  if [[ -n "${NODES}" ]]; then
    NODES+=","
  fi
  NODES+="${HOST}:${port}"
done

usage() {
  cat <<EOF
usage: $0 start|stop|status|smoke|fail-smoke|clean

Environment:
  SERVER=${SERVER}
  REDIS_CLI=${REDIS_CLI}
  HOST=${HOST}
  PORTS="${PORTS}"
  BASE_STATS_PORT=${BASE_STATS_PORT}
  WORKDIR=${WORKDIR}
EOF
}

pid_file_for() {
  local port="$1"
  echo "${WORKDIR}/node_${port}.pid"
}

is_running() {
  local pid_file="$1"
  [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null
}

start_cluster() {
  local index=0
  for port in ${PORTS}; do
    local pid_file
    pid_file="$(pid_file_for "${port}")"
    if is_running "${pid_file}"; then
      echo "node ${HOST}:${port} is already running with pid $(cat "${pid_file}")"
      index=$((index + 1))
      continue
    fi

    local stats_port=$((BASE_STATS_PORT + index))
    local log_file="${WORKDIR}/node_${port}.log"
    local snapshot_file="${WORKDIR}/snapshot_${port}.dat"

    "${SERVER}" \
      --cluster \
      --bind "${HOST}" \
      --port "${port}" \
      --node-addr "${HOST}:${port}" \
      --nodes "${NODES}" \
      --stats-bind "${HOST}" \
      --stats-port "${stats_port}" \
      --snapshot-file "${snapshot_file}" \
      --snapshot-interval 30 >"${log_file}" 2>&1 &
    echo "$!" >"${pid_file}"
    echo "started ${HOST}:${port} pid=$(cat "${pid_file}") stats=${HOST}:${stats_port}"
    index=$((index + 1))
  done

  echo
  echo "cluster nodes: ${NODES}"
  echo "try:"
  echo "  redis-cli -p 6366 CLUSTER INFO"
  echo "  redis-cli -p 6366 CLUSTER NODES"
  echo "  redis-cli -p 6366 CLUSTER SLOTS"
  echo "  redis-cli -p 6366 CLUSTER MYID"
}

stop_cluster() {
  for port in ${PORTS}; do
    local pid_file
    pid_file="$(pid_file_for "${port}")"
    if is_running "${pid_file}"; then
      local pid
      pid="$(cat "${pid_file}")"
      kill -INT "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
      echo "stopped ${HOST}:${port} pid=${pid}"
    fi
    rm -f "${pid_file}"
  done
}

status_cluster() {
  for port in ${PORTS}; do
    local pid_file
    pid_file="$(pid_file_for "${port}")"
    if is_running "${pid_file}"; then
      echo "${HOST}:${port} running pid=$(cat "${pid_file}")"
    else
      echo "${HOST}:${port} stopped"
    fi
  done
}

smoke_cluster() {
  stop_cluster >/dev/null 2>&1 || true
  start_cluster
  sleep 0.5

  local first_port
  first_port="${PORTS%% *}"
  "${REDIS_CLI}" -p "${first_port}" --raw CLUSTER INFO | grep -q 'cluster_known_nodes:3'
  "${REDIS_CLI}" -p "${first_port}" --raw CLUSTER INFO | grep -q 'cluster_slots_assigned:16384'
  "${REDIS_CLI}" -p "${first_port}" --raw CLUSTER NODES | grep -q '0-5461'
  "${REDIS_CLI}" -p "${first_port}" --raw CLUSTER NODES | grep -q '5462-10922'
  "${REDIS_CLI}" -p "${first_port}" --raw CLUSTER NODES | grep -q '10923-16383'
  "${REDIS_CLI}" -p "${first_port}" --raw CLUSTER MYID | grep -Eq '^[0-9a-f]{40}$'

  echo "cluster demo smoke passed"
  stop_cluster
}

fail_smoke_cluster() {
  stop_cluster >/dev/null 2>&1 || true
  start_cluster
  sleep 0.5

  local first_port
  first_port="${PORTS%% *}"
  local victim_port=""
  for port in ${PORTS}; do
    if [[ "${port}" != "${first_port}" ]]; then
      victim_port="${port}"
      break
    fi
  done
  if [[ -z "${victim_port}" ]]; then
    echo "fail-smoke requires at least two ports" >&2
    stop_cluster
    exit 1
  fi

  local victim_pid_file
  victim_pid_file="$(pid_file_for "${victim_port}")"
  if is_running "${victim_pid_file}"; then
    kill -INT "$(cat "${victim_pid_file}")" 2>/dev/null || true
    rm -f "${victim_pid_file}"
  fi

  sleep 7
  "${REDIS_CLI}" -p "${first_port}" --raw CLUSTER INFO | grep -q 'cluster_failed_nodes:1'
  "${REDIS_CLI}" -p "${first_port}" --raw CLUSTER NODES | grep -q "${HOST}:${victim_port} master,fail"

  echo "cluster fail smoke passed"
  stop_cluster
}

case "${1:-}" in
  start)
    start_cluster
    ;;
  stop)
    stop_cluster
    ;;
  status)
    status_cluster
    ;;
  smoke)
    smoke_cluster
    ;;
  fail-smoke)
    fail_smoke_cluster
    ;;
  clean)
    stop_cluster
    rm -rf "${WORKDIR}"
    ;;
  *)
    usage
    exit 2
    ;;
esac
