#!/usr/bin/env bash
set -euo pipefail

SERVER="${SERVER:-build/miniredis}"
REDIS_SERVER="${REDIS_SERVER:-redis-server}"
REDIS_BENCHMARK="${REDIS_BENCHMARK:-redis-benchmark}"
REDIS_CLI="${REDIS_CLI:-redis-cli}"
PORT="${PORT:-19666}"
STATS_PORT="${STATS_PORT:-19080}"
REQUESTS="${REQUESTS:-100000}"
CLIENTS="${CLIENTS:-50}"
RUNS="${RUNS:-3}"
WARMUP_REQUESTS="${WARMUP_REQUESTS:-10000}"
IO_THREADS="${IO_THREADS:-4}"
CACHE_SHARDS="${CACHE_SHARDS:-16}"
BENCH_MATRIX="${BENCH_MATRIX:-0}"
IO_THREADS_LIST="${IO_THREADS_LIST:-}"
CACHE_SHARDS_LIST="${CACHE_SHARDS_LIST:-}"
VALUE_SIZES="${VALUE_SIZES:-64 1024 10240}"
BASELINE_REDIS="${BASELINE_REDIS:-0}"
OUT="${OUT:-build/benchmark-report.txt}"
SUMMARY_OUT="${SUMMARY_OUT:-build/benchmark-summary.md}"
DATA_OUT="${DATA_OUT:-build/benchmark-runs.tsv}"
RAW_OUT="${RAW_OUT:-build/benchmark-raw.txt}"
SAVE_RAW="${SAVE_RAW:-0}"
TMPDIR="$(mktemp -d /tmp/miniredis_bench.XXXXXX)"
PID=""
ACTIVE_SERVER=""

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
  ACTIVE_SERVER=""
}

fail_with_server_log() {
  local message="$1"
  local log_file="$2"
  echo "${message}" >&2
  if [[ -f "${log_file}" ]]; then
    sed -n '1,160p' "${log_file}" >&2
  fi
  exit 1
}

wait_ready() {
  local port="$1"
  local log_file="$2"
  for _ in $(seq 1 100); do
    if ! kill -0 "${PID}" 2>/dev/null; then
      fail_with_server_log "${ACTIVE_SERVER} exited before becoming ready" "${log_file}"
    fi
    if "${REDIS_CLI}" -h 127.0.0.1 -p "${port}" PING 2>/dev/null | grep -q '^PONG$'; then
      if kill -0 "${PID}" 2>/dev/null; then
        return 0
      fi
    fi
    sleep 0.05
  done
  fail_with_server_log "${ACTIVE_SERVER} did not become ready on port ${port}" "${log_file}"
}

start_miniredis() {
  local io_threads="$1"
  local cache_shards="$2"
  local port="$3"
  local stats_port="$4"
  local snapshot="$5"
  local log_file="$6"

  stop_server
  ACTIVE_SERVER="MiniRedis"
  "${SERVER}" \
    --bind 127.0.0.1 \
    --port "${port}" \
    --stats-bind 127.0.0.1 \
    --stats-port "${stats_port}" \
    --io-threads "${io_threads}" \
    --cache-shards "${cache_shards}" \
    --snapshot-file "${snapshot}" \
    --snapshot-interval 3600 >"${log_file}" 2>&1 &
  PID="$!"
  wait_ready "${port}" "${log_file}"
}

start_redis() {
  local port="$1"
  local case_dir="$2"
  local log_file="$3"

  stop_server
  ACTIVE_SERVER="Redis"
  "${REDIS_SERVER}" \
    --bind 127.0.0.1 \
    --port "${port}" \
    --protected-mode no \
    --save "" \
    --appendonly no \
    --dir "${case_dir}" >"${log_file}" 2>&1 &
  PID="$!"
  wait_ready "${port}" "${log_file}"
}

require_positive_integer() {
  local name="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "${name} must be a positive integer: ${value}" >&2
    exit 2
  fi
}

command_version() {
  local command="$1"
  shift
  if command -v "${command}" >/dev/null 2>&1; then
    "${command}" "$@" 2>/dev/null | head -n 1
  else
    echo "not found"
  fi
}

write_headers() {
  mkdir -p "$(dirname "${OUT}")" "$(dirname "${SUMMARY_OUT}")" \
           "$(dirname "${DATA_OUT}")" "$(dirname "${RAW_OUT}")"

  local cpu_model
  local memory_kib
  local server_sha
  cpu_model="$(lscpu 2>/dev/null | awk -F: '/Model name/ {sub(/^[[:space:]]+/, "", $2); print $2; exit}')"
  memory_kib="$(awk '/MemTotal/ {print $2; exit}' /proc/meminfo 2>/dev/null || true)"
  server_sha="$(sha256sum "${SERVER}" 2>/dev/null | awk '{print $1}' || true)"

  {
    echo "# MiniRedis 压测报告"
    echo
    echo "本文件由 scripts/benchmark.sh 生成。结果是本机样本，不代表生产环境上限。"
    echo
    echo "## 测试环境"
    echo
    echo "| 项目 | 值 |"
    echo "| :--- | :--- |"
    echo "| 日期 | $(date -Iseconds) |"
    echo "| 主机 | $(hostname 2>/dev/null || echo unknown) |"
    echo "| 内核 | $(uname -srmo 2>/dev/null || echo unknown) |"
    echo "| CPU | ${cpu_model:-unknown} |"
    echo "| 逻辑 CPU | $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown) |"
    echo "| 内存 KiB | ${memory_kib:-unknown} |"
    echo "| 编译器 | $(command_version "${CXX:-c++}" --version) |"
    echo "| redis-benchmark | $(command_version "${REDIS_BENCHMARK}" --version) |"
    echo "| MiniRedis 二进制 | ${SERVER} |"
    echo "| MiniRedis SHA-256 | ${server_sha:-unknown} |"
    echo
    echo "## 测试参数"
    echo
    echo "| 项目 | 值 |"
    echo "| :--- | :--- |"
    echo "| 每命令每轮请求数 | ${REQUESTS} |"
    echo "| 并发客户端 | ${CLIENTS} |"
    echo "| 正式轮数 | ${RUNS} |"
    echo "| 预热请求数 | ${WARMUP_REQUESTS} |"
    echo "| value 大小 | ${VALUE_SIZES} |"
    echo "| 矩阵模式 | ${BENCH_MATRIX} |"
    echo "| Redis 参考 | ${BASELINE_REDIS} |"
    if [[ "${BENCH_MATRIX}" == "1" ]]; then
      echo "| IO 线程矩阵 | ${IO_THREADS_LIST} |"
      echo "| cache shard 矩阵 | ${CACHE_SHARDS_LIST} |"
    else
      echo "| IO 线程 | ${IO_THREADS} |"
      echo "| cache shards | ${CACHE_SHARDS} |"
    fi
  } >"${OUT}"

  printf 'server\tio_threads\tcache_shards\tvalue_size_bytes\tcommand\trun\trequests_per_sec\tp50_ms\tp95_ms\tp99_ms\tmax_ms\n' >"${DATA_OUT}"

  if [[ "${SAVE_RAW}" == "1" ]]; then
    {
      echo "MiniRedis raw redis-benchmark output"
      echo "date: $(date -Iseconds)"
      echo
    } >"${RAW_OUT}"
  fi
}

append_benchmark_result() {
  local server_name="$1"
  local io_threads="$2"
  local cache_shards="$3"
  local value_size="$4"
  local run="$5"
  local result_file="$6"
  local parsed_file="${result_file}.tsv"

  awk -v server="${server_name}" \
      -v io_threads="${io_threads}" \
      -v cache_shards="${cache_shards}" \
      -v size="${value_size}" \
      -v run="${run}" '
    /======[[:space:]]+(SET|GET)[[:space:]]+======/ {
      for (i = 1; i <= NF; ++i) {
        if ($i == "SET" || $i == "GET") cmd=$i
      }
    }
    /throughput summary:/ { throughput=$3 }
    /latency summary/ { want=1; next }
    want && NF == 6 && $1 ~ /^[0-9.]+$/ {
      printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
             server, io_threads, cache_shards, size, cmd, run,
             throughput, $3, $4, $5, $6);
      want=0
    }
  ' "${result_file}" >"${parsed_file}"

  if [[ "$(wc -l <"${parsed_file}")" -ne 2 ]]; then
    echo "failed to parse SET/GET results from ${result_file}" >&2
    sed -n '1,200p' "${result_file}" >&2
    exit 1
  fi
  cat "${parsed_file}" >>"${DATA_OUT}"
}

run_benchmark_tool() {
  local output_file="$1"
  shift
  local error_file="${output_file}.stderr"
  if ! "${REDIS_BENCHMARK}" "$@" >"${output_file}" 2>"${error_file}"; then
    echo "redis-benchmark failed" >&2
    cat "${error_file}" >&2
    exit 1
  fi
  if [[ -s "${error_file}" ]]; then
    grep -Fvx 'WARNING: Could not fetch server CONFIG' "${error_file}" >&2 || true
  fi
}

run_benchmarks() {
  local server_name="$1"
  local io_threads="$2"
  local cache_shards="$3"
  local port="$4"
  local case_dir="$5"

  for size in ${VALUE_SIZES}; do
    if [[ "${WARMUP_REQUESTS}" -gt 0 ]]; then
      echo "warming ${server_name}: value=${size}B, requests=${WARMUP_REQUESTS}"
      run_benchmark_tool "${case_dir}/warmup_${size}.txt" \
        -h 127.0.0.1 -p "${port}" -q -n "${WARMUP_REQUESTS}" \
        -c "${CLIENTS}" -d "${size}" -t set,get
    fi

    for run in $(seq 1 "${RUNS}"); do
      local result_file="${case_dir}/benchmark_${size}_run${run}.txt"
      echo "measuring ${server_name}: value=${size}B, run=${run}/${RUNS}"
      run_benchmark_tool "${result_file}" \
        -h 127.0.0.1 -p "${port}" -n "${REQUESTS}" \
        -c "${CLIENTS}" -d "${size}" -t set,get

      if [[ "${SAVE_RAW}" == "1" ]]; then
        {
          echo "server: ${server_name}"
          echo "io_threads: ${io_threads}"
          echo "cache_shards: ${cache_shards}"
          echo "value_size_bytes: ${size}"
          echo "run: ${run}"
          cat "${result_file}"
          echo
        } >>"${RAW_OUT}"
      fi

      append_benchmark_result "${server_name}" "${io_threads}" "${cache_shards}" \
        "${size}" "${run}" "${result_file}"
    done
  done
}

run_miniredis_case() {
  local io_threads="$1"
  local cache_shards="$2"
  local port="$3"
  local stats_port="$4"
  local case_dir="${TMPDIR}/miniredis_io${io_threads}_shards${cache_shards}"
  mkdir -p "${case_dir}"

  echo "starting MiniRedis: io_threads=${io_threads}, cache_shards=${cache_shards}"
  start_miniredis "${io_threads}" "${cache_shards}" "${port}" "${stats_port}" \
    "${case_dir}/snapshot.dat" "${case_dir}/server.log"
  run_benchmarks "MiniRedis" "${io_threads}" "${cache_shards}" "${port}" "${case_dir}"
  stop_server
}

run_redis_baseline() {
  local port="$1"
  local case_dir="${TMPDIR}/redis_baseline"
  mkdir -p "${case_dir}"

  if ! command -v "${REDIS_SERVER}" >/dev/null 2>&1; then
    echo "BASELINE_REDIS=1 but redis-server was not found: ${REDIS_SERVER}" >&2
    exit 2
  fi
  echo "starting Redis baseline with persistence disabled"
  start_redis "${port}" "${case_dir}" "${case_dir}/server.log"
  run_benchmarks "Redis" "n/a" "n/a" "${port}" "${case_dir}"
  stop_server
}

write_summary() {
  awk -F '\t' '
    NR == 1 { next }
    {
      key=$1 SUBSEP $2 SUBSEP $3 SUBSEP $4 SUBSEP $5
      if (!(key in seen)) {
        seen[key]=1
        order[++count]=key
        server[key]=$1
        io[key]=$2
        shards[key]=$3
        size[key]=$4
        command[key]=$5
        min_qps[key]=$7
      }
      runs[key]++
      sum_qps[key]+=$7
      sumsq_qps[key]+=$7*$7
      if ($7 < min_qps[key]) min_qps[key]=$7
      sum_p50[key]+=$8
      sum_p95[key]+=$9
      sum_p99[key]+=$10
      if ($11 > worst_max[key]) worst_max[key]=$11
    }
    BEGIN {
      print "| server | io_threads | cache_shards | value bytes | command | runs | avg req/s | min req/s | QPS CV | avg p50 ms | avg p95 ms | avg p99 ms | worst max ms |"
      print "| :--- | ---: | ---: | ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
    }
    END {
      for (i=1; i<=count; ++i) {
        key=order[i]
        avg=sum_qps[key]/runs[key]
        variance=sumsq_qps[key]/runs[key]-avg*avg
        if (variance < 0) variance=0
        cv=avg > 0 ? sqrt(variance)/avg*100 : 0
        printf("| %s | %s | %s | %s | %s | %d | %.2f | %.2f | %.2f%% | %.3f | %.3f | %.3f | %.3f |\n",
               server[key], io[key], shards[key], size[key], command[key], runs[key],
               avg, min_qps[key], cv, sum_p50[key]/runs[key],
               sum_p95[key]/runs[key], sum_p99[key]/runs[key], worst_max[key])
      }
    }
  ' "${DATA_OUT}" >"${SUMMARY_OUT}"

  {
    echo
    echo "## 聚合结果"
    echo
    cat "${SUMMARY_OUT}"
    echo
    echo "QPS CV 是多轮 QPS 的变异系数，数值越低表示轮次间越稳定。"
    echo
    echo "## 逐轮结果"
    echo
    echo "| server | io_threads | cache_shards | value bytes | command | run | req/s | p50 ms | p95 ms | p99 ms | max ms |"
    echo "| :--- | ---: | ---: | ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: |"
    awk -F '\t' 'NR > 1 {
      printf("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
             $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)
    }' "${DATA_OUT}"
    echo
    echo "## 解读边界"
    echo
    echo "- Redis 行是关闭 RDB/AOF 后的同机参考，不表示两者功能完全等价。"
    echo "- 虚拟化、CPU 频率、宿主机负载、分配器和内核设置都会显著影响结果。"
    echo "- 应将 p95/p99、轮次波动与吞吐一起解读，不单独展示 QPS。"
  } >>"${OUT}"
}

require_positive_integer REQUESTS "${REQUESTS}"
require_positive_integer CLIENTS "${CLIENTS}"
require_positive_integer RUNS "${RUNS}"
if [[ ! "${WARMUP_REQUESTS}" =~ ^[0-9]+$ ]]; then
  echo "WARMUP_REQUESTS must be a non-negative integer: ${WARMUP_REQUESTS}" >&2
  exit 2
fi
if [[ ! -x "${SERVER}" ]]; then
  echo "MiniRedis server is not executable: ${SERVER}" >&2
  exit 2
fi
if ! command -v "${REDIS_BENCHMARK}" >/dev/null 2>&1; then
  echo "redis-benchmark was not found: ${REDIS_BENCHMARK}" >&2
  exit 2
fi

if [[ "${BENCH_MATRIX}" == "1" ]]; then
  IO_THREADS_LIST="${IO_THREADS_LIST:-1 4}"
  CACHE_SHARDS_LIST="${CACHE_SHARDS_LIST:-1 16}"
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
    run_miniredis_case "${io_threads}" "${cache_shards}" \
      "${case_port}" "${case_stats_port}"
    case_index=$((case_index + 1))
  done
done

if [[ "${BASELINE_REDIS}" == "1" ]]; then
  run_redis_baseline "$((PORT + case_index * 10))"
fi

write_summary

echo "benchmark report written to ${OUT}"
echo "aggregate summary written to ${SUMMARY_OUT}"
echo "machine-readable runs written to ${DATA_OUT}"
if [[ "${SAVE_RAW}" == "1" ]]; then
  echo "raw redis-benchmark output written to ${RAW_OUT}"
else
  echo "set SAVE_RAW=1 to retain full redis-benchmark output"
fi
