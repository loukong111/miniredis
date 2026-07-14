# 测试与压测

## CTest

CMake 会注册单元测试；如果本机存在 `bash`、`redis-cli` 和 `curl`，还会注册集成冒烟测试：

```bash
ctest --test-dir build --output-on-failure
```

示例输出：

```text
1/2 Test #1: miniredis_unit_tests ............. Passed
2/2 Test #2: miniredis_integration_smoke ...... Passed
```

## CI

项目提供 GitHub Actions CI，位于 `.github/workflows/ci.yml`。CI 会在 Ubuntu 24.04 上执行：

- Release 构建、单元测试和 redis-cli/curl 集成冒烟测试
- recovery/soak smoke，覆盖强杀恢复、坏快照回退和监控端点
- Debug + ASan/UBSan 单元测试
- Docker image build 检查

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
```

其中 `migrate-smoke` 会启动三节点集群，写入属于首节点的 key，执行 `CLUSTER MIGRATE <slot> <target-node>`，再验证源节点 key 数归零、目标节点可读到迁移后的 value。
`replica_demo.sh smoke` 会先启动 master 写入历史数据，再启动 replica 验证启动全量同步；随后验证写命令能复制到 replica，并验证 replica 普通写请求返回 `READONLY`。

## 单元测试覆盖

单元测试覆盖：

- RESP 半包解析和粘包解析
- CacheStore 基础读写、sharded cache、TTL 查询、TTL 惰性删除、TTL cleanup 释放内存池 block
- maxmemory noeviction / lru 淘汰策略
- TTL-aware 二进制快照读写、V1 快照兼容、异常 entry count 拒绝加载
- AOF 增量日志 replay、坏尾忽略、rewrite 中断残留文件清理、rewrite buffer 超限重试、二进制 value、TTL 绝对过期时间和命令层写 AOF
- AUTH 未认证拒绝、错误密码拒绝、正确密码放行，以及 ACL readonly/readwrite/admin、用户启停、命令白名单/黑名单、key 前缀权限和 `ACL WHOAMI/LIST/GETUSER`
- key/value 大小限制、资源保护配置解析和监控导出
- `SETNX`、`APPEND`、`STRLEN`、`TYPE`、`INCR/DECR`、`GETDEL`、`GETEX`、`SET EX`、`EXPIRE/PEXPIRE`、`PERSIST`、`TTL/PTTL`、`MGET`、`INFO`、`SLOWLOG` 和 COMMAND 元信息响应
- 简化主从复制的全量快照响应、backlog/PSYNC 增量同步、内部复制命令、replica 只读保护和 `INFO replication`
- Redis Cluster hash slot、固定 slot 映射表、MOVED/ASK 统计、slot 路由一致性、`MEET/FORGET` 拓扑维护、`SETSLOT` 状态切换和手动 failover takeover
- 动态线程池 stop 后拒绝继续提交任务

## 性能压测

压测脚本：

```bash
scripts/benchmark.sh
```

一次完整样本和环境说明见 [benchmark-report.md](benchmark-report.md)。

默认会使用一组参数运行：

```bash
REQUESTS=100000 CLIENTS=50 IO_THREADS=4 CACHE_SHARDS=16 scripts/benchmark.sh
```

对比多 Reactor 和分片 CacheStore 的收益时，使用矩阵模式：

```bash
BENCH_MATRIX=1 \
IO_THREADS_LIST="1 4" \
CACHE_SHARDS_LIST="1 16" \
VALUE_SIZES="64 1024" \
scripts/benchmark.sh
```

矩阵模式会依次启动不同配置的 MiniRedis，使用 `redis-benchmark` 跑 `SET/GET`，并输出带 `io_threads`、`cache_shards`、QPS 和 p50/p95/p99/max 延迟的 Markdown 表格。

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

默认参数：

```text
requests: 100000
clients: 50
io_threads: 4
cache_shards: 16
value sizes: 64B, 1KB, 10KB
output: build/benchmark-report.txt
summary: build/benchmark-summary.md
```

当前环境一次样本结果：

```text
io_threads  cache_shards  value_size_bytes  command  requests/sec  p50 ms  p95 ms  p99 ms  max ms
4           16            64                SET      29922.20      1.551   2.879   3.831   8.631
4           16            64                GET      29455.08      1.583   2.823   3.671   6.791
4           16            1024              SET      27540.62      1.687   3.055   4.055   13.871
4           16            1024              GET      28344.67      1.663   2.959   3.847   8.031
```

这个结果只作为当前开发机样本，不代表生产性能。延迟展示使用 p50/p95/p99/max，避免只看中位数掩盖尾延迟。正式评估建议固定 CPU 频率、独占 CPU、跑多轮取均值，并补充 Redis 基线对比。
