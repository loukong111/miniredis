# 并发模型与安全性

本文说明 MiniRedis 的并发模型、主要共享数据结构和当前安全边界。目标是让项目不只停留在“能跑”，而是能解释为什么多客户端、持久化和监控同时运行时不会互相踩内存。

## 线程模型

```text
main thread
  -> MiniRedisServer lifecycle
  -> accept reactor
       -> accept client fd
       -> round-robin dispatch to worker reactor

worker reactor thread
  -> epoll_wait
  -> Scheduler ready queue
  -> coroutine client session
  -> RESP decode / CommandHandler / RESP encode

background threads
  -> snapshot task through DynamicThreadPool
  -> cleanup/stats loop
  -> cluster heartbeat / slot map sync
  -> optional AOF rewrite thread
```

连接 fd 只归属一个 worker reactor。客户端读写和 RESP 解析发生在该连接所属的 worker 线程内，避免同一个 fd 被多个线程同时读写。

## CacheStore

`CacheStore` 按 key hash 拆分为多个 shard：

- 每个 shard 有独立 `std::shared_mutex`
- `GET` / `EXISTS` / `TTL` / `PTTL` / `TYPE` 使用读路径语义，发现过期 key 时走惰性删除
- `SET` / `SETNX` / `APPEND` / `INCR` / `DEL` / `GETDEL` / `GETEX` / `EXPIRE` / `PEXPIRE` / `PERSIST` 使用独占锁修改命中的 shard
- `snapshot()` 遍历所有 shard，并在每个 shard 内持有共享锁复制当前存活数据
- `cleanup()` 遍历所有 shard，并在每个 shard 内独占删除过期数据

这种设计让单 key 操作只锁一个 shard，降低多客户端访问不同 key 时的锁竞争。

## 内存池

64B 小 value 使用 `FixedMemoryPool`：

- `allocate()` / `deallocate()` 内部由 mutex 保护 free list
- `CacheStore` 负责在 key 删除、过期和覆盖时释放旧 block
- 大 value 走堆分配，不进入固定块池

内存池只管理小块复用，不承担对象生命周期之外的并发控制；对象归属仍由 `CacheStore` shard 锁保护。

## 持久化

### 快照

快照任务读取 `CacheStore::snapshot()` 得到内存数据副本，然后在后台线程写文件：

```text
CacheStore snapshot copy
  -> write snapshot.tmp
  -> flush + fsync
  -> backup old snapshot to .bak
  -> rename snapshot.tmp to snapshot.dat
  -> fsync parent directory
```

因此 snapshot 写文件阶段不长期持有 CacheStore 锁。定时快照由 `snapshot_running_` 防重入，上一轮未完成时会跳过本轮，避免多个线程同时写同一个快照文件。

### AOF

AOF append 由 `AppendOnlyFile::mutex_` 串行保护：

- 命令写入 fd
- 按 `appendfsync` 策略刷盘
- 如果 rewrite 正在运行，同时追加到 rewrite buffer

`BGREWRITEAOF` 后台线程先基于当前 snapshot 写 compact 临时文件；收尾时循环把 rewrite 期间的新写入 buffer 交换到局部变量，并在锁外写入临时文件。只有当 buffer 为空时，才短暂持有 AOF mutex 执行最终 `fsync + rename + reopen`，从而减少高写入场景下的写路径阻塞。rewrite 失败会删除临时文件并保留旧 AOF；rewrite buffer 有上限，超限时中止本轮 rewrite，但业务写入仍追加到旧 AOF。

已覆盖的恢复场景：

- AOF 坏尾：忽略最后一条不完整 RESP 命令，保留前面完整命令
- rewrite 中断：残留 `.rewrite.tmp` 不参与 replay，不污染正式 AOF
- rewrite buffer 超限：中止本轮 rewrite，保留旧 AOF，并允许下一次 `BGREWRITEAOF` 重试
- snapshot 损坏：主快照标记 `.bad`，尝试 `.bak` 回退

## Cluster 状态

`ClusterSlotMap` 内部有 mutex：

- slot owner 读写
- slot migrating/importing/stable 状态
- node healthy/pfail/fail 状态
- epoch 更新

命令层访问 slot map 时还会通过 server 持有的 `slot_map_mutex_` 保护跨对象操作，例如迁移、failover takeover 和拓扑持久化回调。当前实现是简化 cluster，不提供 Redis Cluster 完整分布式一致性协议。

## 统计与可观测性

`Stats` 大部分计数器使用 atomic：

- 命令数、连接数、延迟样本、内存指标
- ready 状态
- snapshot / AOF rewrite 状态
- replication 在线副本、ACK offset、待追平量、重连和错误计数

慢日志使用 mutex 保护 deque，节点地址使用 mutex 保护字符串。`/stats`、`/metrics` 和 `INFO` 读取的是运行时快照，允许轻微滞后，换取低开销。

## 当前边界

- 当前复制将网络发送移出客户端线程，由每副本独立 worker 复用长连接并按 offset 追平；不提供完整 Redis PSYNC2、backlog 持久化和自动选主。
- 手动 failover takeover 会更新本地 slot ownership，但不做多数派投票。
- AOF rewrite 采用单进程后台线程实现，避免依赖 `fork`，更贴合当前 C++ 单进程架构。
- HTTP stats server 是轻量监控入口，不适合作为高 QPS 业务接口。
- 没有 TLS，公网部署需要网关、内网隔离或安全组。

## 验证方式

基础验证：

```bash
cmake --build build -j
./build/miniredis_unit_tests
ctest --test-dir build -R miniredis_unit_tests --output-on-failure
```

恢复和压力验证：

```bash
scripts/recovery_soak.sh
scripts/resource_failure_soak.sh
scripts/benchmark.sh
```

建议额外使用 sanitizer 构建：

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIREDIS_ENABLE_ASAN=ON \
  -DMINIREDIS_ENABLE_UBSAN=ON
cmake --build build-sanitize -j
./build-sanitize/miniredis_unit_tests
```

如果要专门查数据竞争，可以单独使用 TSAN 构建：

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIREDIS_ENABLE_TSAN=ON
cmake --build build-tsan -j
./build-tsan/miniredis_unit_tests
```

ASAN 和 TSAN 不能同时开启。TSAN 更适合在低并发网络集成测试和 recovery/soak 场景下辅助排查数据竞争。
