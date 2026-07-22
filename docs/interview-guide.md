# 面试讲解指南

这份文档用于面试前复盘 MiniRedis。重点不是逐行背代码，而是把项目定位、技术亮点、工程取舍和边界讲清楚。

## 一句话介绍

MiniRedis 是一个用 C++20 coroutine + epoll 实现的轻量级内网 KV 缓存中间件，支持 RESP、TTL、snapshot/AOF、HTTP/Prometheus 监控、轻量 ACL、replication/backlog、cluster slot 路由和静态三节点多数派故障转移。

## 项目定位

建议这样介绍：

> 我没有把它定位成 Redis 替代品，而是面向中小型后台服务、本地或内网环境的轻量 KV 缓存中间件。项目重点是把 C++ 后端常见能力串起来：异步网络、协议解析、并发 KV 存储、持久化恢复、资源保护、可观测性、测试和简单集群路由。

这个说法比“仿 Redis”更稳，因为它强调场景和工程边界。

## 90 秒项目介绍

> 我做的 MiniRedis 是一个 Linux 上的轻量内网 KV 缓存服务。网络层用 C++20 协程和 epoll 实现多 Reactor，accept reactor 分发连接，worker reactor 负责非阻塞 RESP 读写；存储层用 sharded CacheStore 和 64B 内存池降低锁竞争和小对象分配开销。可靠性方面做了 TTL-aware snapshot、AOF rewrite、坏尾恢复和损坏快照回退。复制使用 replid + offset + backlog 判断增量续传，三节点模式通过 epoch 持久化投票选主，master 丢失多数派后会隔离写入。项目还有 Prometheus 指标、Qt 管理端、CTest/故障脚本和多轮压测报告。它不是完整 Redis，重点是展示 C++ 服务端从请求到运维的完整工程链路。

这 90 秒只讲主线。面试官对某个点感兴趣时，再进入 Scheduler 生命周期、AOF rewrite 并发写或 failover 投票细节。

## 核心亮点

面试中优先讲这 6 点：

1. 网络模型：C++20 coroutine + epoll，多 Reactor，accept reactor 分发 fd，worker reactor 处理客户端 IO。
2. 存储模型：CacheStore 按 key hash 分片，每个 shard 独立 `shared_mutex`，降低多客户端读写锁竞争。
3. 内存管理：64B 固定块内存池复用小 value，大 value 走堆分配。
4. 持久化：TTL-aware snapshot + AOF，snapshot 使用 `tmp + fsync + rename`，AOF rewrite 合并增量 buffer。
5. 可观测性：`/stats`、`/metrics`、`INFO`、SLOWLOG，暴露连接、延迟、资源限制、snapshot、AOF rewrite 状态。
6. 集群与 HA 演示：CRC16 slot、`MOVED/ASK`、slot 迁移、节点 `pfail/fail`、多数派自动选主和旧主写隔离。

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
- 自动选主只面向固定小规模成员组，没有成员动态变更和自动副本补充。
- 没有完整 PSYNC2 双 ID 历史和复制积压缓冲持久化。
- 没有 Redis Cluster gossip、Sentinel 兼容或 Raft 日志复制。
- slot 迁移是简化同步实现，不提供完整回滚。

这不是扣分，反而说明你知道边界。

### 复制怎么做的？

回答思路：

- master 每次进程启动生成新的 40 位 replication ID；replica 持久化 ID + offset，并随 `REPLPSYNC` 一起发送。
- 只有 ID 一致且 backlog 命中时才拉取缺失写命令；ID 变化、backlog 不足或 offset 无效时退回 `REPLFULLSYNC`。
- master 写入成功后只追加 backlog 并通知后台 worker，客户端线程不等待副本网络；每个副本使用独立长连接按 offset 发送并读取 ACK。
- 长连接的 `REPLACK/REPLAPPLY` 也携带 replication ID，旧 master 连接会被拒绝；replica 对重复 offset 幂等应答，对不连续 offset 返回 `REPLGAP`。
- replica 普通写请求返回 `READONLY`。
- replica 启动全量同步后会触发本地 AOF rewrite，压缩本地日志。
- 自动模式连续探测 master，选择 offset 最大的健康副本发起 election；epoch、投票和 leader 原子持久化，每个 epoch 最多投一票。
- master 失去多数派后返回 `MASTERDOWN`，旧 master 发现更高 epoch leader 后降级并重新同步，降低 split-brain 风险。

### 怎么证明性能数据可信？

回答思路：

- 不使用单次峰值；脚本默认预热后跑三轮。
- 报告同时保留平均 QPS、最低 QPS、QPS 变异系数和 p50/p95/p99/worst max。
- 记录 CPU、内核、编译器、客户端数、value 大小和二进制 SHA-256，避免结果脱离环境。
- 可选同机 Redis 参考，Redis 关闭 RDB/AOF，MiniRedis 关闭 AOF 并避免压测期间 snapshot。
- Redis 只是参考坐标，不把结果表述为严格的功能等价性对比。

### 为什么没有做完整 Redis？

回答思路：

- 项目目标是展示 C++ 后端工程能力，不是重写 Redis。
- 完整 Redis 涉及大量数据结构、协议兼容、Lua、RDB、AOF 多进程 rewrite、Sentinel、Cluster gossip 等，范围太大。
- 当前项目聚焦内网 KV 缓存中间件常见能力：网络、并发、持久化、监控、资源保护和基础 HA 演示。

## 演示流程

完整的时间线、讲解话术和备用方案见 [MiniRedis 8 分钟演示指南](demo-guide.md)。

推荐演示顺序：

1. 启动 Qt Console。
2. 通过服务管理入口启动单节点，设置 `requirepass`、ACL 用户、AOF、maxmemory、slowlog。
3. 在命令工作区执行 `AUTH`、`SET`、`SETNX`、`GET`、`GETDEL`、`GETEX`、`APPEND`、`STRLEN`、`TYPE`、`INCR`、`EXPIRE/PEXPIRE`、`PERSIST`、`TTL/PTTL`、`MGET`。
4. 通过运行指标入口展示连接数、命令数、命中率、延迟、snapshot/AOF 状态。
5. 通过诊断工具触发 `/healthz`、`/readyz`、`INFO persistence`、`ACL LIST/GETUSER`、`SLOWLOG GET`。
6. 在服务管理中执行持久化演示、重写 AOF、重启服务，验证恢复。
7. 启动三节点集群演示，在集群路由入口展示 `CLUSTER NODES/SLOTS/SLOTMAP`、MOVED 自动跟随和 slot migration。
8. 在演示中心运行自动故障转移脚本，展示多数派 election、新主写入、旧主降级和数据追平。
9. 进入压测入口运行一组 SET/GET 压测，展示 QPS 和 p95/p99。

## 简历描述

可用于简历的精简版本：

- 基于 C++20 coroutine + epoll 实现多 Reactor 异步网络模型，支持多客户端非阻塞读写、RESP 半包/粘包解析和 redis-cli 交互。
- 实现轻量级 KV 缓存核心，支持 TTL/PTTL、GETDEL/GETEX、MGET、SETNX、APPEND、STRLEN、TYPE、INCR/DECR、EXPIRE/PEXPIRE/PERSIST、maxmemory、LRU、64B 小对象内存池和 sharded CacheStore。
- 实现 TTL-aware snapshot + AOF 持久化，支持原子快照落盘、AOF rewrite、坏尾恢复和损坏 snapshot 回退。
- 提供 HTTP `/stats`、Prometheus `/metrics`、SLOWLOG 和 Qt Console，可视化展示运行状态、资源限制、延迟和持久化状态。
- 实现 experimental Redis Cluster 风格 slot 路由和轻量 HA，支持 `MOVED/ASK`、slot 迁移、epoch 持久化投票、多数派自动选主与写隔离。
- 接入 CTest、集成 smoke、recovery/soak 和 benchmark 脚本，输出 QPS 与 p50/p95/p99/max 延迟报告。

## 不足与后续优化

如果面试官问“还有什么能优化”，可以答：

- 增加 `perf` 火焰图、CPU 亲和性和独立物理机压测，定位协议解析、系统调用和锁竞争的占比。
- 增加 TSAN 网络集成测试，验证多客户端、snapshot、AOF rewrite 并发场景。
- 完善 replication：增加 `replid2` 切换历史、backlog 持久化和更完整的 PSYNC2 兼容。
- 完善 failover：增加成员动态变更、自动副本补充和更严格的 leader lease/共识模型。
- 增加更多数据结构，例如 hash/list，但优先级低于稳定性和可观测性。
- 引入 TLS 或和网关集成，增强公网部署安全性。

## 稳妥表述

推荐说：

> 这个项目不是完整 Redis，而是面向内网后台服务的轻量 KV 缓存中间件。我重点做了异步网络、并发 KV、持久化恢复、资源保护、可观测性和实验性 cluster 路由，并用单测、集成测试、恢复脚本和压测报告证明主要行为。

避免说：

> 我实现了一个 Redis。

后者容易被追问完整协议、RDB、Sentinel、Cluster gossip、Lua、AOF 多进程 rewrite，风险很高。
