#!/usr/bin/env bash
set -euo pipefail

SERVER="${SERVER:-build/miniredis}"
REDIS_BENCHMARK="${REDIS_BENCHMARK:-redis-benchmark}"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
PORT="${PORT:-19666}"
STATS_PORT="${STATS_PORT:-19080}"
REQUESTS="${REQUESTS:-100000}"
CLIENTS="${CLIENTS:-50}"
IO_THREADS="${IO_THREADS:-4}"
CACHE_SHARDS="${CACHE_SHARDS:-16}"
BENCH_MATRIX="${BENCH_MATRIX:-0}"
IO_THREADS_LIST="${IO_THREADS_LIST:-}"
CACHE_SHARDS_LIST="${CACHE_SHARDS_LIST:-}"
VALUE_SIZES="${VALUE_SIZES:-64 1024 10240}"
OUT="${OUT:-build/benchmark-report.txt}"
SUMMARY_OUT="${SUMMARY_OUT:-build/benchmark-summary.md}"
RAW_OUT="${RAW_OUT:-build/benchmark-raw.txt}"
SAVE_RAW="${SAVE_RAW:-0}"
TMPDIR="$(mktemp -d /tmp/miniredis_bench.XXXXXX)"
PID=""

cleanup() {
  stop_server
  rm -rf "${TMPDIR}"
}
trap cleanup EXIT

stop_server() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill -INT "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  PID=""
}

wait_ready() {
  local port="$1"
  for _ in $(seq 1 100); do
    if "${REDIS_CLI}" -p "${port}" PING 2>/dev/null | grep -q PONG; then
      return 0
    fi
    sleep 0.05
  done
  echo "miniredis did not become ready on port ${port}" >&2
  if [[ -f "${TMPDIR}/server.log" ]]; then
    sed -n '1,120p' "${TMPDIR}/server.log" >&2
  fi
  exit 1
}

start_server() {
  local io_threads="$1"
  local cache_shards="$2"
  local port="$3"
  local stats_port="$4"
  local snapshot="$5"
  local log_file="$6"

  stop_server
  "${SERVER}" \
    --bind 127.0.0.1 \
    --port "${port}" \
    --stats-bind 127.0.0.1 \
    --stats-port "${stats_port}" \
    --io-threads "${io_threads}" \
    --cache-shards "${cache_shards}" \
    --snapshot-file "${snapshot}" \
    --snapshot-interval 60 >"${log_file}" 2>&1 &
  PID="$!"
  wait_ready "${port}"
}

write_headers() {
  mkdir -p "$(dirname "${OUT}")" "$(dirname "${SUMMARY_OUT}")" "$(dirname "${RAW_OUT}")"

  {
    echo "MiniRedis benchmark"
    echo "date: $(date -Iseconds)"
    echo "server: ${SERVER}"
    echo "requests: ${REQUESTS}"
    echo "clients: ${CLIENTS}"
    echo "value_sizes: ${VALUE_SIZES}"
    echo "bench_matrix: ${BENCH_MATRIX}"
    if [[ "${BENCH_MATRIX}" == "1" ]]; then
      echo "io_threads_list: ${IO_THREADS_LIST}"
      echo "cache_shards_list: ${CACHE_SHARDS_LIST}"
    else
      echo "io_threads: ${IO_THREADS}"
      echo "cache_shards: ${CACHE_SHARDS}"
    fi
    echo
    echo "| io_threads | cache_shards | value_size_bytes | command | requests/sec | p50 ms | p95 ms | p99 ms | max ms |"
    echo "| ---: | ---: | ---: | :--- | ---: | ---: | ---: | ---: | ---: |"
  } >"${OUT}"

  {
    echo "| io_threads | cache_shards | value_size_bytes | command | requests/sec | p50 ms | p95 ms | p99 ms | max ms |"
    echo "| ---: | ---: | ---: | :--- | ---: | ---: | ---: | ---: | ---: |"
  } >"${SUMMARY_OUT}"

  if [[ "${SAVE_RAW}" == "1" ]]; then
    {
      echo "MiniRedis raw benchmark"
      echo "date: $(date -Iseconds)"
      echo "server: ${SERVER}"
      echo "requests: ${REQUESTS}"
      echo "clients: ${CLIENTS}"
      echo "value_sizes: ${VALUE_SIZES}"
      echo "bench_matrix: ${BENCH_MATRIX}"
      echo
    } >"${RAW_OUT}"
  fi
}

append_benchmark_result() {
  local io_threads="$1"
  local cache_shards="$2"
  local value_size="$3"
  local result_file="$4"

  awk -v io_threads="${io_threads}" \
      -v cache_shards="${cache_shards}" \
      -v size="${value_size}" '
    /======[[:space:]]+(SET|GET)[[:space:]]+======/ {
      for (i = 1; i <= NF; ++i) {
        if ($i == "SET" || $i == "GET") cmd=$i
      }
    }
    /throughput summary:/ { throughput=$3 }
    /latency summary/ { want=1; next }
    want && NF == 6 && $1 ~ /^[0-9.]+$/ {
      printf("| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
             io_threads, cache_shards, size, cmd, throughput, $3, $4, $5, $6);
      want=0
    }
  ' "${result_file}" | tee -a "${SUMMARY_OUT}" >>"${OUT}"
}

run_case() {
  local io_threads="$1"
  local cache_shards="$2"
  local port="$3"
  local stats_port="$4"
  local case_dir="${TMPDIR}/io${io_threads}_shards${cache_shards}"
  mkdir -p "${case_dir}"

  echo "starting miniredis: io_threads=${io_threads}, cache_shards=${cache_shards}"
  start_server "${io_threads}" "${cache_shards}" "${port}" "${stats_port}" \
    "${case_dir}/snapshot.dat" "${case_dir}/server.log"

  for size in ${VALUE_SIZES}; do
    local result_file="${case_dir}/benchmark_${size}.txt"
    echo "running redis-benchmark: io_threads=${io_threads}, cache_shards=${cache_shards}, value=${size}B"
    "${REDIS_BENCHMARK}" -p "${port}" -n "${REQUESTS}" -c "${CLIENTS}" -d "${size}" -t set,get >"${result_file}"

    if [[ "${SAVE_RAW}" == "1" ]]; then
      {
        echo "io_threads: ${io_threads}"
        echo "cache_shards: ${cache_shards}"
        echo "value_size_bytes: ${size}"
        cat "${result_file}"
        echo
      } >>"${RAW_OUT}"
    fi

    append_benchmark_result "${io_threads}" "${cache_shards}" "${size}" "${result_file}"
  done

  stop_server
}

if [[ "${BENCH_MATRIX}" == "1" ]]; then
  if [[ -z "${IO_THREADS_LIST}" ]]; then
    IO_THREADS_LIST="1 4"
  fi
  if [[ -z "${CACHE_SHARDS_LIST}" ]]; then
    CACHE_SHARDS_LIST="1 16"
  fi
else
  IO_THREADS_LIST="${IO_THREADS}"
  CACHE_SHARDS_LIST="${CACHE_SHARDS}"
fi

write_headers

case_index=0
for io_threads in ${IO_THREADS_LIST}; do
  for cache_shards in ${CACHE_SHARDS_LIST}; do
    case_port=$((PORT + case_index * 10))
    case_stats_port=$((STATS_PORT + case_index * 10))
    run_case "${io_threads}" "${cache_shards}" "${case_port}" "${case_stats_port}"
    case_index=$((case_index + 1))
  done
done

echo "benchmark report written to ${OUT}"
echo "benchmark summary written to ${SUMMARY_OUT}"
if [[ "${SAVE_RAW}" == "1" ]]; then
  echo "raw benchmark log written to ${RAW_OUT}"
else
  echo "set SAVE_RAW=1 to also write the full redis-benchmark log to ${RAW_OUT}"
fi
