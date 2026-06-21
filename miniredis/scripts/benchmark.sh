#!/usr/bin/env bash
set -euo pipefail

SERVER="${SERVER:-build/miniredis}"
REDIS_BENCHMARK="${REDIS_BENCHMARK:-redis-benchmark}"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
PORT="${PORT:-19666}"
STATS_PORT="${STATS_PORT:-19080}"
REQUESTS="${REQUESTS:-100000}"
CLIENTS="${CLIENTS:-50}"
VALUE_SIZES="${VALUE_SIZES:-64 1024 10240}"
OUT="${OUT:-build/benchmark-report.txt}"
SUMMARY_OUT="${SUMMARY_OUT:-build/benchmark-summary.md}"
RAW_OUT="${RAW_OUT:-build/benchmark-raw.txt}"
SAVE_RAW="${SAVE_RAW:-0}"
TMPDIR="$(mktemp -d /tmp/miniredis_bench.XXXXXX)"
PID=""

cleanup() {
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill -INT "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  rm -rf "${TMPDIR}"
}
trap cleanup EXIT

wait_ready() {
  for _ in $(seq 1 100); do
    if "${REDIS_CLI}" -p "${PORT}" PING 2>/dev/null | grep -q PONG; then
      return 0
    fi
    sleep 0.05
  done
  echo "miniredis did not become ready on port ${PORT}" >&2
  exit 1
}

mkdir -p "$(dirname "${OUT}")" "$(dirname "${SUMMARY_OUT}")" "$(dirname "${RAW_OUT}")"

"${SERVER}" \
  --bind 127.0.0.1 \
  --port "${PORT}" \
  --stats-bind 127.0.0.1 \
  --stats-port "${STATS_PORT}" \
  --snapshot-file "${TMPDIR}/snapshot.dat" \
  --snapshot-interval 60 >"${TMPDIR}/server.log" 2>&1 &
PID="$!"
wait_ready

{
  echo "MiniRedis benchmark"
  echo "date: $(date -Iseconds)"
  echo "server: ${SERVER}"
  echo "requests: ${REQUESTS}"
  echo "clients: ${CLIENTS}"
  echo "value_sizes: ${VALUE_SIZES}"
  echo
  echo "| value_size_bytes | command | requests/sec | p50 ms | p95 ms | p99 ms | max ms |"
  echo "| ---: | :--- | ---: | ---: | ---: | ---: | ---: |"
} >"${OUT}"

{
  echo "| value_size_bytes | command | requests/sec | p50 ms | p95 ms | p99 ms | max ms |"
  echo "| ---: | :--- | ---: | ---: | ---: | ---: | ---: |"
} >"${SUMMARY_OUT}"

if [[ "${SAVE_RAW}" == "1" ]]; then
  {
    echo "MiniRedis raw benchmark"
    echo "date: $(date -Iseconds)"
    echo "server: ${SERVER}"
    echo "requests: ${REQUESTS}"
    echo "clients: ${CLIENTS}"
    echo "value_sizes: ${VALUE_SIZES}"
    echo
  } >"${RAW_OUT}"
fi

for size in ${VALUE_SIZES}; do
  result_file="${TMPDIR}/benchmark_${size}.txt"
  echo "running redis-benchmark with ${size}B values..."
  "${REDIS_BENCHMARK}" -p "${PORT}" -n "${REQUESTS}" -c "${CLIENTS}" -d "${size}" -t set,get >"${result_file}"

  if [[ "${SAVE_RAW}" == "1" ]]; then
    {
      echo "value_size_bytes: ${size}"
      cat "${result_file}"
      echo
    } >>"${RAW_OUT}"
  fi

  awk -v size="${size}" '
    /======[[:space:]]+(SET|GET)[[:space:]]+======/ {
      for (i = 1; i <= NF; ++i) {
        if ($i == "SET" || $i == "GET") cmd=$i
      }
    }
    /throughput summary:/ { throughput=$3 }
    /latency summary/ { want=1; next }
    want && NF == 6 && $1 ~ /^[0-9.]+$/ {
      printf("| %s | %s | %s | %s | %s | %s | %s |\n",
             size, cmd, throughput, $3, $4, $5, $6);
      want=0
    }
  ' "${result_file}" | tee -a "${SUMMARY_OUT}" >>"${OUT}"
done

echo "benchmark report written to ${OUT}"
echo "benchmark summary written to ${SUMMARY_OUT}"
if [[ "${SAVE_RAW}" == "1" ]]; then
  echo "raw benchmark log written to ${RAW_OUT}"
else
  echo "set SAVE_RAW=1 to also write the full redis-benchmark log to ${RAW_OUT}"
fi
