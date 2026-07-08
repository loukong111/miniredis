# 压测报告

本文记录一次可复现的 MiniRedis 单机压测样本。结果用于展示项目工程验证方式，不代表生产环境上限。

## 测试环境

| 项目 | 值 |
| :--- | :--- |
| 日期 | 2026-06-21 20:31:45 +08:00 |
| 操作系统 | Ubuntu on Linux 7.0.0-22-generic |
| 运行环境 | VMware virtual machine |
| CPU | 8 vCPU, 13th Gen Intel Core i5-13500H |
| 内存 | 5.3 GiB |
| 编译器 | g++ 15.2.0 |
| CMake | 4.2.3 |
| 构建类型 | Release |

## 测试参数

| 项目 | 值 |
| :--- | ---: |
| 单命令请求数 | 100000 |
| 并发客户端数 | 50 |
| IO 线程数 | 4 |
| CacheStore 分片数 | 16 |
| value 大小 | 64B, 1KB, 10KB |
| 压测工具 | redis-benchmark |
| 输出文件 | build/benchmark-report.txt |

命令：

```bash
REQUESTS=100000 CLIENTS=50 IO_THREADS=4 CACHE_SHARDS=16 \
VALUE_SIZES="64 1024 10240" scripts/benchmark.sh
```

## 测试结果

| value_size_bytes | command | requests/sec | p50 ms | p95 ms | p99 ms | max ms |
| ---: | :--- | ---: | ---: | ---: | ---: | ---: |
| 64 | SET | 29922.20 | 1.551 | 2.879 | 3.831 | 8.631 |
| 64 | GET | 29455.08 | 1.583 | 2.823 | 3.671 | 6.791 |
| 1024 | SET | 27540.62 | 1.687 | 3.055 | 4.055 | 13.871 |
| 1024 | GET | 28344.67 | 1.663 | 2.959 | 3.847 | 8.031 |
| 10240 | SET | 15015.02 | 3.279 | 4.663 | 5.663 | 10.327 |
| 10240 | GET | 21376.66 | 2.127 | 3.911 | 4.967 | 11.039 |

## 结果解读

- 64B value 下，SET/GET 都接近 30k req/s，p99 延迟约 3.7-3.8 ms。
- 1KB value 下吞吐下降不明显，说明当前主要瓶颈不是 value 拷贝本身。
- 10KB value 下 SET 降到约 15k req/s，GET 约 21k req/s，符合大 value 网络写回和内存拷贝成本上升的预期。
- p95/p99 比 p50 更适合展示服务尾延迟，README 和脚本都保留 p50/p95/p99/max。

## 复现方式

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
scripts/benchmark.sh
```

矩阵对比：

```bash
BENCH_MATRIX=1 \
IO_THREADS_LIST="1 4" \
CACHE_SHARDS_LIST="1 16" \
VALUE_SIZES="64 1024" \
scripts/benchmark.sh
```

## 测试限制

- 本结果来自虚拟机环境，CPU 调度、宿主机负载和虚拟网络都会影响数据。
- 当前报告未固定 CPU 频率，也未做 CPU 亲和性绑定。
- 当前报告未展示 CPU 使用率、RSS、上下文切换和网络吞吐。
- 正式性能评估建议跑多轮取均值，并补充 Redis 基线对比。

## 后续测试方向

后续更完整的性能报告建议补充：

- `io_threads=1/2/4/8` 对比
- `cache_shards=1/4/16/64` 对比
- `appendfsync=no/everysec/always` 对写入延迟影响
- 长时间 soak 下 RSS 是否稳定
- 与 Redis 单机在相同机器、相同参数下的基线对比
