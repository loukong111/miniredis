# 压测报告

本文记录一次可复现的 MiniRedis 单机多轮压测，并提供关闭持久化的 Redis 同机参考。结果用于证明测试方法和当前实现的性能量级，不代表生产环境上限。

## 测试环境

| 项目 | 值 |
| :--- | :--- |
| 日期 | 2026-07-22 16:54 +08:00 |
| 操作系统 | Ubuntu, Linux 7.0.0-27-generic |
| 运行环境 | VMware virtual machine |
| CPU | 8 vCPU, 13th Gen Intel Core i5-13500H |
| 内存 | 7.2 GiB |
| 编译器 | g++ 15.2.0 |
| 构建类型 | Release |
| redis-benchmark | 8.0.5 |
| Redis 参考 | 8.0.5, RDB/AOF 关闭 |

`scripts/benchmark.sh` 还会在生成报告中记录主机、内核、二进制 SHA-256 和完整逐轮数据，便于判断两份结果是否来自同一构建。

## 测试参数

| 项目 | 值 |
| :--- | ---: |
| 每条命令每轮请求数 | 100000 |
| 并发客户端数 | 50 |
| 预热请求数 | 10000 |
| 正式轮数 | 3 |
| MiniRedis IO 线程 | 4 |
| CacheStore 分片数 | 16 |
| value 大小 | 64B, 1KB, 10KB |

MiniRedis 未开启 AOF，snapshot interval 设为 3600 秒，避免测量期间触发快照。Redis 参考进程使用 `--save "" --appendonly no`。

复现命令：

```bash
RUNS=3 WARMUP_REQUESTS=10000 BASELINE_REDIS=1 \
REQUESTS=100000 CLIENTS=50 \
IO_THREADS=4 CACHE_SHARDS=16 \
VALUE_SIZES="64 1024 10240" \
scripts/benchmark.sh
```

## 聚合结果

| server | value bytes | command | runs | avg req/s | min req/s | QPS CV | avg p50 ms | avg p95 ms | avg p99 ms | worst max ms |
| :--- | ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| MiniRedis | 64 | SET | 3 | 22225.74 | 21258.50 | 3.08% | 1.210 | 2.276 | 3.018 | 35.999 |
| MiniRedis | 64 | GET | 3 | 23037.65 | 21598.27 | 5.56% | 1.167 | 2.132 | 2.991 | 26.607 |
| MiniRedis | 1024 | SET | 3 | 22198.80 | 20479.21 | 6.60% | 1.204 | 2.212 | 2.970 | 43.455 |
| MiniRedis | 1024 | GET | 3 | 24164.21 | 22316.45 | 8.86% | 1.154 | 2.130 | 3.268 | 50.239 |
| MiniRedis | 10240 | SET | 3 | 18627.46 | 18024.51 | 2.45% | 1.466 | 2.498 | 3.271 | 29.647 |
| MiniRedis | 10240 | GET | 3 | 20064.56 | 17927.57 | 11.01% | 1.356 | 2.370 | 3.770 | 80.319 |
| Redis | 64 | SET | 3 | 36857.90 | 35100.04 | 3.94% | 0.724 | 1.228 | 1.762 | 4.703 |
| Redis | 64 | GET | 3 | 35522.03 | 34891.84 | 1.35% | 0.738 | 1.476 | 2.242 | 37.279 |
| Redis | 1024 | SET | 3 | 34752.20 | 33333.33 | 2.91% | 0.762 | 1.642 | 2.490 | 5.991 |
| Redis | 1024 | GET | 3 | 34072.78 | 33489.62 | 1.22% | 0.772 | 1.511 | 2.479 | 33.471 |
| Redis | 10240 | SET | 3 | 25652.56 | 24408.10 | 4.59% | 0.983 | 1.954 | 3.042 | 7.135 |
| Redis | 10240 | GET | 3 | 27265.85 | 26434.05 | 4.01% | 0.956 | 1.754 | 2.711 | 48.063 |

QPS CV 是三轮 QPS 的变异系数，数值越低表示轮次间越稳定。`worst max` 只是三轮中最大的单次延迟，对虚拟机调度很敏感，需与 p95/p99 和 CV 一起解读。

## 结果解读

- 64B value 下 MiniRedis SET/GET 平均约为 22.2k/23.0k req/s，平均 p99 约 3ms。
- 1KB value 下 QPS 没有显著下降，10KB 下 SET/GET 降至约 18.6k/20.1k req/s，大 value 的拷贝和 socket 输出开销开始更明显。
- MiniRedis 在本次样本中达到 Redis 参考 QPS 的约 60%-74%；value 越大，两者的相对差距越小。
- MiniRedis 各项平均 p99 为 2.97-3.77ms，Redis 参考为 1.76-3.04ms。
- MiniRedis QPS CV 为 2.45%-11.01%。10KB GET 的波动和 80ms worst max 说明该虚拟机样本存在明显调度噪声，不应只挑选最快一轮展示。

MiniRedis 与 Redis 的差距符合当前实现阶段：Redis 拥有成熟的协议热路径、内存分配器和网络输出优化；MiniRedis 的请求路径还包含通用 RESP 对象、标准容器分配、ACL/路由检查和逐命令指标。这是基于实现和数据的推断，要确认各部分占比仍需 `perf` 火焰图。

## 输出文件

| 文件 | 用途 |
| :--- | :--- |
| `build/benchmark-report.txt` | 环境、参数、聚合和逐轮数据 |
| `build/benchmark-summary.md` | 便于粘贴的聚合 Markdown 表格 |
| `build/benchmark-runs.tsv` | 便于二次分析的逐轮 TSV |
| `build/benchmark-raw.txt` | 仅在 `SAVE_RAW=1` 时生成的完整工具输出 |

## 测试边界

- 样本来自虚拟机，未固定 CPU 频率，未做 CPU 亲和性和独占 CPU。
- 客户端和服务端在同一台主机，测到的不是真实跨机网络延迟。
- 当前报告没有同时采集 CPU 使用率、RSS、上下文切换和网络吞吐。
- Redis 参考与 MiniRedis 的命令语义、数据结构、内存管理和成熟度不同，不能将表格解读为完整产品对比。

## 下一步性能工作

- 在独立 Linux 物理机上设置 CPU 亲和性，重复相同脚本。
- 使用 `perf record/report` 和火焰图定位 RESP 解析、协程调度、hash/锁和 socket 读写占比。
- 对比 `io_threads=1/2/4/8` 和 `cache_shards=1/4/16/64`，确认拐点而不是默认认为线程越多越快。
- 分别测量 AOF `no/everysec/always` 的吞吐、p99 和数据丢失窗口。
- 执行长时间 soak，联合观察 RSS、内存池命中和延迟漂移。
