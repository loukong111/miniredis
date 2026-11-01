# 测试与压测

## CTest

Linux 默认启用测试，CMake 按本机工具注册以下用例：

| CTest 用例 | 覆盖范围 | 额外依赖 |
| :--- | :--- | :--- |
| `miniredis_unit_tests` | 存储、协议、配置、权限、持久化和复制语义 | 无 |
| `miniredis_integration_smoke` | RESP/HTTP、认证、恢复与集群命令 | Bash、redis-cli、curl |
| `miniredis_failover_smoke` | 三节点选举、旧主降级与 quorum 写隔离 | Bash、redis-cli、curl |
| `miniredis_protocol_limits` | 原始报文、协议和缓冲区边界 | Python 3 |

后三项还需开启 `MINIREDIS_ENABLE_INTEGRATION_TESTS`，并构建服务端。网络用例需要允许创建本地 socket。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

只验证单元测试时使用 `ctest --test-dir build -R '^miniredis_unit_tests$' --output-on-failure`。

## CI

项目提供 GitHub Actions CI，位于 `.github/workflows/ci.yml`。CI 会在 Ubuntu 24.04 上执行：

- Release 构建、单元测试和 redis-cli/curl 集成冒烟测试
- recovery/soak smoke，覆盖强杀恢复、坏快照回退和监控端点
- resource/failure smoke，覆盖 maxmemory、LRU、AOF rewrite、AOF 坏尾和 rewrite tmp 清理
- Debug + ASan/UBSan 全套 CTest，包括三节点自动故障转移和原始协议边界测试
- Docker image build、服务端与客户端构建边界检查
- 桌面端 TypeScript 检查、构建、协议与进程测试，以及 Xvfb 中的 Electron 界面验证

## 桌面端验证

先构建服务端，再在 `tools/desktop` 执行：

```bash
npm ci
MINIREDIS_TEST_BINARY=../../build/miniredis npm test
npm run build
npm run test:desktop
```

无桌面的 Linux 环境使用 `xvfb-run -a npm run test:desktop`。只有在 CI 容器无法提供 Chromium 沙箱时，为测试命令设置 `MINIREDIS_TEST_NO_SANDBOX=1`；应用日常启动不需要该参数。

后端测试覆盖 RESP 拆包、重复响应、UTF-8 字节长度、非法帧、脚本预校验、AUTH 脱敏、MOVED/ASK 对应命令、认证失败、超时与取消、密码存储、本地实例和任务进程组清理。真实服务测试读取 RESP、监控与复制信息。

界面验证通过 Playwright 启动 Electron 和真实主从实例，操作 Monaco、运行多行命令、核对重复响应、剪贴板、监控与 Prometheus、复制拓扑、主题及本地实例视图，并验证退出后服务进程已经结束。默认使用临时用户数据目录，测试截图通过 `MINIREDIS_SCREENSHOTS` 指定输出目录；不会读取个人连接或密码。

## 集成测试覆盖

集成测试脚本位于 `tests/integration/smoke.sh`，覆盖：

- 启动带 AUTH 的服务
- 验证未认证请求被拒绝
- 验证 `SET`、`SETNX`、`SET EX`、`GET`、`GETDEL`、`GETEX`、`MGET`、`APPEND`、`STRLEN`、`TYPE`、`INCR/DECR`、`DEL`、`EXISTS`、`EXPIRE/PEXPIRE`、`PERSIST`、`TTL/PTTL`、`INFO`、`SLOWLOG`
- 查询 `/healthz`、`/readyz`、`/stats` 和 `/metrics`，验证资源限制、snapshot 和 AOF rewrite 指标暴露
- 停服后重启，验证快照恢复
- 启动单节点 cluster 模式
- 验证 `CLUSTER INFO`、`CLUSTER NODES`、`CLUSTER MYID`、`CLUSTER KEYSLOT`、`CLUSTER COUNTKEYSINSLOT`、`CLUSTER SLOTS`、`CLUSTER SLOTMAP`、`CLUSTER MEET/FORGET`
- 验证 cluster 模式下基础 `SET/GET`

三节点 cluster demo 还提供独立 smoke：

```bash
scripts/cluster_demo.sh smoke
scripts/cluster_demo.sh fail-smoke
scripts/cluster_demo.sh migrate-smoke
scripts/replica_demo.sh smoke
scripts/failover_demo.sh smoke
```

其中 `migrate-smoke` 会启动三节点集群，写入属于首节点的 key，执行 `CLUSTER MIGRATE <slot> <target-node>`，再验证源节点 key 数归零、目标节点可读到迁移后的 value。
`replica_demo.sh smoke` 会先验证启动同步和 replica 只读保护，再停止 replica、在 master 写入数据并重启 replica，验证主写入不等待离线副本且 replica 能通过 backlog 自动追平。随后脚本会重启 master，并刻意让新旧生命周期的 offset 保持相同，验证 replication ID 变化仍会强制全量同步，避免错误续传。
`failover_demo.sh smoke` 会启动一个 master 和两个 replica，停止 master 后验证 epoch/多数派 election、新主写入与副本同步；旧 master 重启后必须识别新 leader、降级并追平，且普通写入必须返回 `READONLY`。

## 单元测试覆盖

单元测试覆盖：

- RESP 半包解析和粘包解析
- CacheStore 基础读写、sharded cache、TTL 查询、TTL 惰性删除、TTL cleanup 释放内存池 block
- maxmemory noeviction / lru 淘汰策略、零额度 shard 和动态降低额度
- TTL-aware 二进制快照读写、V1 快照兼容、异常 entry count 拒绝加载
- AOF 增量日志 replay、坏尾忽略、rewrite 中断残留文件清理、rewrite buffer 超限重试、二进制 value、TTL 绝对过期时间和命令层写 AOF
- AUTH 未认证拒绝、错误密码与带额外零字节的密码拒绝、正确密码放行，以及 ACL readonly/readwrite/admin、用户启停、命令白名单/黑名单、key 前缀权限和 `ACL WHOAMI/LIST/GETUSER`
- key/value 大小限制、资源保护配置解析和监控导出
- `SETNX`、`APPEND`、`STRLEN`、`TYPE`、`INCR/DECR`、`GETDEL`、`GETEX`、`SET EX`、`EXPIRE/PEXPIRE`、`PERSIST`、`TTL/PTTL`、`MGET`、`INFO`、`SLOWLOG` 和 COMMAND 元信息响应
- 主从复制的全量快照响应、replication ID 状态文件、backlog/PSYNC 增量同步、旧 master 推送拒绝、replica 只读保护、failover epoch 持久化投票、quorum 写隔离和 `INFO replication`
- Redis Cluster hash slot、固定 slot 映射表、MOVED/ASK 统计、slot 路由一致性、`MEET/FORGET` 拓扑维护、`SETSLOT` 状态切换和手动 failover takeover
- 动态线程池 stop 后拒绝继续提交任务

## Sanitizer

内存与未定义行为检查：

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIREDIS_ENABLE_ASAN=ON \
  -DMINIREDIS_ENABLE_UBSAN=ON
cmake --build build-sanitize -j
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-sanitize --output-on-failure
```

排查数据竞争时，可单独创建 Debug 构建并设置 `-DMINIREDIS_ENABLE_TSAN=ON`。ASan 与 TSan 不能同时开启；sanitizer 检查不能替代并发语义和故障恢复验证。

## 性能压测

压测脚本：

```bash
scripts/benchmark.sh
```

一次完整样本和环境说明见 [benchmark-report.md](benchmark-report.md)。

默认会先预热，再使用同一组参数运行三轮：

```bash
RUNS=3 WARMUP_REQUESTS=10000 \
REQUESTS=100000 CLIENTS=50 \
IO_THREADS=4 CACHE_SHARDS=16 \
scripts/benchmark.sh
```

脚本保留每轮 TSV 数据，聚合报告输出平均 QPS、最低 QPS、QPS 变异系数（CV）、平均 p50/p95/p99 和各轮最大延迟。这比单轮结果更容易发现虚拟机调度或宿主机负载造成的抖动。

同机 Redis 参考：

```bash
RUNS=3 BASELINE_REDIS=1 \
REQUESTS=100000 CLIENTS=50 \
VALUE_SIZES="64 1024 10240" \
scripts/benchmark.sh
```

Redis 参考进程会关闭 RDB 和 AOF；MiniRedis 该压测也不启用 AOF，并将 snapshot interval 设为 3600 秒。这是相同客户端参数下的本机参考，不代表两者功能完全等价。

对比多 Reactor 和分片 CacheStore 的收益时，使用矩阵模式：

```bash
BENCH_MATRIX=1 \
IO_THREADS_LIST="1 4" \
CACHE_SHARDS_LIST="1 16" \
VALUE_SIZES="64 1024" \
scripts/benchmark.sh
```

矩阵模式会依次启动不同配置的 MiniRedis，使用 `redis-benchmark` 跑 `SET/GET`，用来验证 IO reactor 数和 CacheStore shard 数的影响。

输出文件：

| 路径 | 内容 |
| :--- | :--- |
| `build/benchmark-report.txt` | 环境、参数、聚合结果、逐轮结果和解读边界 |
| `build/benchmark-summary.md` | 只包含便于粘贴的聚合 Markdown 表格 |
| `build/benchmark-runs.tsv` | 机器可读的每轮原始指标 |
| `build/benchmark-raw.txt` | `SAVE_RAW=1` 时保留完整 `redis-benchmark` 输出 |

## 恢复与稳定性测试

可靠性演示脚本：

```bash
scripts/recovery_soak.sh
```

脚本会启动本地服务，写入普通 key 和 TTL key，等待快照落盘后执行 `kill -9`，重启验证数据和 TTL 恢复；随后故意破坏主快照，验证 `.bad` 标记和 `.bak` 回退；最后跑一轮短压测并检查 `/healthz`、`/readyz`、`/stats`、`/metrics` 和日志。

可通过环境变量调整规模：

```bash
REQUESTS=100000 CLIENTS=50 SOAK_SECONDS=60 scripts/recovery_soak.sh
```

资源与故障压测脚本：

```bash
scripts/resource_failure_soak.sh
```

这个脚本更偏上线前检查，会覆盖：

- `maxmemory noeviction` 下超大写入返回 OOM
- `maxmemory lru` 下高频写入触发 evicted_keys
- AOF rewrite 完成后重启恢复
- AOF 坏尾被忽略，坏尾命令不会生效
- 残留 `.rewrite.tmp` 启动时被清理，旧 AOF 不被错误替换
- 短时并发压测后检查 `/healthz`、`/readyz`、`/stats`、`/metrics`

默认参数比较温和，适合作为 CI smoke；上线前可以放大：

```bash
REQUESTS=100000 CLIENTS=50 scripts/resource_failure_soak.sh
```

历史压测结果统一保存在 [压测报告](benchmark-report.md)，避免使用方法与样本数据混杂。容量评估应在目标环境固定负载条件后重新运行。
