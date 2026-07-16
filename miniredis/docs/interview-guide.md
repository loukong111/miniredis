# 面试讲解指南

这份文档用于面试前复盘 MiniRedis。重点不是逐行背代码，而是把项目定位、技术亮点、工程取舍和边界讲清楚。

## 一句话介绍

MiniRedis 是一个用 C++20 coroutine + epoll 实现的轻量级内网 KV 缓存中间件，支持 RESP 协议、TTL、snapshot/AOF 持久化、HTTP/Prometheus 监控、轻量 ACL、资源保护、简化 replication/backlog 增量同步和 experimental cluster slot 路由。

## 项目定位

建议这样介绍：

> 我没有把它定位成 Redis 替代品，而是面向中小型后台服务、本地或内网环境的轻量 KV 缓存中间件。项目重点是把 C++ 后端常见能力串起来：异步网络、协议解析、并发 KV 存储、持久化恢复、资源保护、可观测性、测试和简单集群路由。

这个说法比“仿 Redis”更稳，因为它强调场景和工程边界。

## 核心亮点

面试中优先讲这 6 点：

1. 网络模型：C++20 coroutine + epoll，多 Reactor，accept reactor 分发 fd，worker reactor 处理客户端 IO。
2. 存储模型：CacheStore 按 key hash 分片，每个 shard 独立 `shared_mutex`，降低多客户端读写锁竞争。
3. 内存管理：64B 固定块内存池复用小 value，大 value 走堆分配。
4. 持久化：TTL-aware snapshot + AOF，snapshot 使用 `tmp + fsync + rename`，AOF rewrite 合并增量 buffer。
5. 可观测性：`/stats`、`/metrics`、`INFO`、SLOWLOG，暴露连接、延迟、资源限制、snapshot、AOF rewrite 状态。
6. 集群演示：CRC16 slot、固定 slot map、`MOVED/ASK`、`MEET/FORGET` 拓扑维护、slot 迁移、节点 `pfail/fail`、手动 failover takeover。

## 架构讲解

可以按这条链路讲：

```text
client
  -> listen socket
  -> accept reactor
  -> worker reactor
  -> Scheduler ready queue
  -> coroutine client session
  -> RESP decoder
  -> AUTH / ACL
  -> cluster route
  -> CommandHandler
  -> CacheStore / AOF / Replication / Stats
  -> RESP encoder
  -> non-blocking write
```

关键点：

- fd 只归属一个 worker reactor，避免多个线程同时读写同一个连接。
- 协程用于把非阻塞 IO 写成接近同步的控制流。
- ready queue 让 scheduler 不只是 IO waiter，也能管理可运行协程。
- 每个 worker reactor 独立 epoll 和 eventfd，停机时 eventfd 唤醒 epoll。

## 常见问题

### 为什么用 coroutine + epoll？

回答思路：

- epoll 负责高并发 fd 事件通知。
- coroutine 把读写流程从回调风格改成顺序风格，可读性更好。
- 多 Reactor 避免所有连接挤在一个 epoll 线程里。
- 项目里没有追求复杂框架，而是手写 scheduler，更适合展示底层理解。

### Scheduler 和 IO awaiter 有什么区别？

回答思路：

- 如果只有 `epoll -> resume`，更像 IO awaiter。
- 当前 scheduler 维护 ready queue，也能主动调度 ready task。
- `eventfd` 用于跨线程唤醒 epoll，支持优雅停机和任务调度。

### CacheStore 怎么保证线程安全？

回答思路：

- key 先 hash 到 shard。
- 每个 shard 有独立 `shared_mutex`。
- GET/TTL/PTTL/STRLEN/TYPE 等读路径走轻量访问，SET/SETNX/APPEND/INCR/DEL/GETDEL/GETEX/EXPIRE/PEXPIRE/PERSIST 使用独占锁修改命中的 shard。
- 过期 key 使用惰性删除，避免过期数据长期占内存。
- snapshot 遍历 shard，复制存活数据后释放锁，文件写入不长期阻塞业务读写。

### 为什么设计 64B 内存池？

回答思路：

- 缓存系统常见小 value 很多，频繁 malloc/free 容易带来开销和碎片。
- 64B 固定块简单可控，适合展示内存复用。
- 大 value 仍走堆分配，避免内存池复杂化。
- 内存池只做分配复用，生命周期由 CacheStore 管理。

### 快照怎么保证原子性？

回答思路：

保存流程：

```text
write snapshot.tmp
flush
fsync(tmp)
backup old snapshot to .bak
rename(tmp, snapshot)
fsync(parent dir)
```

如果主 snapshot 损坏，加载时移动为 `.bad`，再尝试 `.bak` 回退。

### AOF rewrite 怎么处理并发写？

回答思路：

- AOF append 由 mutex 串行保护。
- rewrite 基于当前 CacheStore snapshot 写 compact 临时文件。
- rewrite 期间新写入继续追加旧 AOF，同时进入 rewrite buffer。
- rewrite buffer 有上限，超限时中止本轮 rewrite，但旧 AOF 继续可用。
- 收尾阶段分批交换 rewrite buffer，在锁外写入临时文件，只在最终 `fsync + rename + reopen` 时短暂持有 AOF mutex。
- 失败时删除临时文件，记录最后状态和错误原因，下一次 `BGREWRITEAOF` 可以重试。

### AOF 坏尾怎么办？

回答思路：

- replay 使用 RESP decoder 按完整命令解析。
- 如果末尾是不完整 RESP，忽略坏尾并保留前面的完整命令。
- 单测覆盖了 `完整 AOF + 半截 RESP 尾巴` 的恢复场景。

### 为什么要做 `/stats` 和 `/metrics`？

回答思路：

- 服务上线后不只要能处理请求，还要能排障。
- `/stats` 给 Qt Console 和人读，`/metrics` 给 Prometheus。
- 指标包含命令数、命中率、连接数、延迟、内存池、资源保护、snapshot、AOF rewrite 状态。

### Cluster 做到了什么？

回答思路：

- 使用 Redis Cluster CRC16 计算 slot。
- 维护 16384 slots 到 node 的固定映射表。
- 非本节点 key 返回 `MOVED`。
- slot 迁移时源节点返回 `ASK`，目标节点需要先收 `ASKING`。
- 支持 `CLUSTER INFO/NODES/SLOTS/SLOTMAP/KEYSLOT/COUNTKEYSINSLOT/MEET/FORGET`。
- 支持简化 `CLUSTER MIGRATE` 和手动 `CLUSTER FAILOVER TAKEOVER`。

### Cluster 没做到什么？

要主动说清楚：

- 没有完整 Redis Cluster gossip。
- 没有多数派投票和自动选主。
- 没有完整 PSYNC2、replication offset 多副本比较和复制积压缓冲持久化。
- slot 迁移是简化同步实现，不提供完整回滚。

这不是扣分，反而说明你知道边界。

### 复制怎么做的？

回答思路：

- replica 启动时先带本地 offset 请求 `REPLPSYNC`，backlog 命中时只拉取缺失写命令。
- backlog 不足时退回 `REPLFULLSYNC` 做全量同步。
- master 成功执行 `SET/SETNX/APPEND/INCR/DEL/GETDEL/GETEX/EXPIRE/PEXPIRE/PERSIST` 后写入 replication backlog，并通过 `REPLAPPLY <offset> ...` 转发内部复制命令。
- replica 普通写请求返回 `READONLY`。
- replica 启动全量同步后会触发本地 AOF rewrite，压缩本地日志。

### 为什么没有做完整 Redis？

回答思路：

- 项目目标是展示 C++ 后端工程能力，不是重写 Redis。
- 完整 Redis 涉及大量数据结构、协议兼容、Lua、RDB、AOF 多进程 rewrite、Sentinel、Cluster gossip 等，范围太大。
- 当前项目聚焦内网 KV 缓存中间件常见能力：网络、并发、持久化、监控、资源保护和基础 HA 演示。

## 演示流程

推荐演示顺序：

1. 启动 Qt Console。
2. 通过服务管理入口启动单节点，设置 `requirepass`、ACL 用户、AOF、maxmemory、slowlog。
3. 在命令工作区执行 `AUTH`、`SET`、`SETNX`、`GET`、`GETDEL`、`GETEX`、`APPEND`、`STRLEN`、`TYPE`、`INCR`、`EXPIRE/PEXPIRE`、`PERSIST`、`TTL/PTTL`、`MGET`。
4. 通过运行指标入口展示连接数、命令数、命中率、延迟、snapshot/AOF 状态。
5. 通过诊断工具触发 `/healthz`、`/readyz`、`INFO persistence`、`ACL LIST/GETUSER`、`SLOWLOG GET`。
6. 在服务管理中执行持久化演示、重写 AOF、重启服务，验证恢复。
7. 启动三节点集群演示，在集群路由入口展示 `CLUSTER NODES/SLOTS/SLOTMAP`、`MEET/FORGET`、MOVED 自动跟随、slot migration 和手动 takeover。
8. 在演示中心运行 smoke、recovery/soak、replica、cluster 脚本，说明核心场景可自动验证。
9. 进入压测入口运行一组 SET/GET 压测，展示 QPS 和 p95/p99。

## 简历描述

可用于简历的精简版本：

- 基于 C++20 coroutine + epoll 实现多 Reactor 异步网络模型，支持多客户端非阻塞读写、RESP 半包/粘包解析和 redis-cli 交互。
- 实现轻量级 KV 缓存核心，支持 TTL/PTTL、GETDEL/GETEX、MGET、SETNX、APPEND、STRLEN、TYPE、INCR/DECR、EXPIRE/PEXPIRE/PERSIST、maxmemory、LRU、64B 小对象内存池和 sharded CacheStore。
- 实现 TTL-aware snapshot + AOF 持久化，支持原子快照落盘、AOF rewrite、坏尾恢复和损坏 snapshot 回退。
- 提供 HTTP `/stats`、Prometheus `/metrics`、SLOWLOG 和 Qt Console，可视化展示运行状态、资源限制、延迟和持久化状态。
- 实现 experimental Redis Cluster 风格 slot 路由，支持 `MOVED/ASK`、slot 迁移、节点故障标记和手动 failover takeover。
- 接入 CTest、集成 smoke、recovery/soak 和 benchmark 脚本，输出 QPS 与 p50/p95/p99/max 延迟报告。

## 不足与后续优化

如果面试官问“还有什么能优化”，可以答：

- 补 Redis 基线压测，对比 MiniRedis 和 Redis 在相同机器上的 QPS/延迟差异。
- 增加 TSAN 网络集成测试，验证多客户端、snapshot、AOF rewrite 并发场景。
- 完善 replication backlog：增加持久化 backlog、offset 校验和更完整的 PSYNC2 兼容。
- 完善 failover：基于 epoch、offset 和多数派确认做自动选主。
- 增加更多数据结构，例如 hash/list，但优先级低于稳定性和可观测性。
- 引入 TLS 或和网关集成，增强公网部署安全性。

## 稳妥表述

推荐说：

> 这个项目不是完整 Redis，而是面向内网后台服务的轻量 KV 缓存中间件。我重点做了异步网络、并发 KV、持久化恢复、资源保护、可观测性和实验性 cluster 路由，并用单测、集成测试、恢复脚本和压测报告证明主要行为。

避免说：

> 我实现了一个 Redis。

后者容易被追问完整协议、RDB、Sentinel、Cluster gossip、Lua、AOF 多进程 rewrite，风险很高。
