# MiniRedis

MiniRedis 是一个面向本地和内网后台服务的轻量级 KV 缓存中间件，使用 C++20、coroutine、epoll 和 CMake 实现。围绕网络 IO、协议解析、并发控制、持久化恢复、监控和 experimental cluster 路由做工程化练习与演示。

## 项目亮点

- C++20 coroutine + epoll 多 Reactor 网络模型，accept reactor 分发连接，worker reactor 负责客户端 IO。
- 支持 RESP 协议和常用 String/KV 命令：`PING`、`AUTH`、`ACL`、`SET/SETNX`、`GET/GETDEL/GETEX`、`MGET`、`APPEND`、`STRLEN`、`TYPE`、`INCR/DECR`、`INCRBY/DECRBY`、`DEL`、`EXISTS`、`EXPIRE/PEXPIRE/PERSIST`、`TTL/PTTL`、`COMMAND`、`INFO`、`SLOWLOG`、`BGREWRITEAOF`。
- CacheStore 按 key hash 分片，结合 `std::shared_mutex` 降低多客户端读写锁竞争。
- 64B 固定块内存池复用小 value，大 value 走堆分配。
- 支持 TTL、惰性删除、后台 cleanup、`maxmemory` 和 `noeviction/lru` 淘汰策略。
- 支持 TTL-aware 二进制 snapshot、AOF 增量日志、AOF rewrite、坏尾恢复、rewrite 失败重试和损坏 snapshot 回退。
- 提供 `/healthz`、`/readyz`、`/stats` 和 Prometheus `/metrics`，暴露连接、延迟、资源限制、snapshot、AOF rewrite 等指标。
- 支持 AUTH、轻量 ACL 用户、命令权限、key 前缀权限、安全默认 bind、连接数限制、请求大小限制、优雅停机、Docker 和 systemd 部署示例。
- 提供简化 replication、轻量 replication backlog/PSYNC 增量同步、experimental Redis Cluster 风格 slot 路由、`MOVED/ASK`、slot 迁移和手动 failover takeover。
- 提供 Qt Console，可视化演示资源管理器、命令工作区、服务启停、Replication、Cluster、诊断监控和压测。

## 平台定位

MiniRedis 服务端面向 Linux 内网服务器部署，网络层使用 `epoll/eventfd/POSIX socket`；Qt Console 是跨平台管理端，通过 RESP/HTTP 连接本地、远程或 Docker 中的服务端。

| 模块 | Linux | Windows | macOS |
|---|---:|---:|---:|
| MiniRedis 服务端 | 支持 | 不支持 | 不支持 |
| Qt Console 管理端 | 支持 | 支持/计划支持 | 支持/计划支持 |
| Docker 运行服务端 | 支持 | 通过 Docker Desktop | 通过 Docker Desktop |
| systemd 部署 | 支持 | 不适用 | 不适用 |

构建边界：

- Linux 默认构建服务端和测试。
- Windows/macOS 推荐通过 Docker 运行服务端，或连接远程 Linux 服务端。
- 非 Linux 默认关闭服务端和测试；如需构建 Qt Console，使用 `-DMINIREDIS_BUILD_QT_CONSOLE=ON`。
- 如果在 Windows/macOS 强制开启 `MINIREDIS_BUILD_SERVER=ON`，CMake 会给出明确错误提示。

详细平台边界见 [docs/platform.md](docs/platform.md)。

## 快速开始

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/miniredis --requirepass change-me
```

另开终端：

```bash
redis-cli -p 6366 -a change-me PING
redis-cli -p 6366 -a change-me SET foo bar
redis-cli -p 6366 -a change-me GET foo
redis-cli -p 6366 -a change-me INFO memory

curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/metrics
```

Docker:

```bash
export MINIREDIS_REQUIREPASS='change-me'
docker compose up --build
```

更多构建、配置和部署说明见 [docs/usage.md](docs/usage.md)。

## Qt Console

Qt Console 是项目的可视化演示入口，采用类似数据库客户端的工作台布局，尽量减少对终端命令的依赖：

- 顶部菜单和工具栏：提供连接、运行、服务、集群、监控、工具等入口，主界面保持命令工作区优先，低频能力通过弹窗打开。
- 资源管理器：左侧展示当前 MiniRedis 连接、KV 命令、观测诊断、集群和 ACL 常用命令，支持双击插入模板。
- 命令工作区：中间提供多行命令编辑器、命令模板、历史记录、选中/当前行执行、全部执行和统一输出面板。
- 服务与集群：可视化启动单节点、master/replica、三节点 cluster，配置 AOF、ACL 用户、maxmemory、IO threads、cache shards，并演示 `MEET/FORGET`、`MOVED/ASK`、slot 迁移、节点故障和手动 takeover。
- 监控诊断：展示 `/stats`、Prometheus `/metrics`、`/healthz`、`/readyz`、`INFO`、`ACL WHOAMI/LIST/GETUSER`、`SLOWLOG` 等运行和排障信息。
- 自动验证：可从 Qt 端触发 smoke、recovery/soak、replica、cluster 脚本，并在演示日志中查看输出。
- 压测：调用 `redis-benchmark` 和 `scripts/benchmark.sh` 输出 QPS 与延迟数据。

构建运行：

```bash
cmake -S . -B build-qt \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINIREDIS_BUILD_QT_CONSOLE=ON \
  -DMINIREDIS_ENABLE_INTEGRATION_TESTS=OFF

cmake --build build-qt -j
./build-qt/tools/qt_console/miniredis_qt_console
```

在 Windows/macOS 上只构建 Qt Console 时，推荐显式关闭服务端和测试：

```bash
cmake -S . -B build-qt \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINIREDIS_BUILD_SERVER=OFF \
  -DMINIREDIS_BUILD_TESTS=OFF \
  -DMINIREDIS_BUILD_QT_CONSOLE=ON

cmake --build build-qt -j
```

![Qt 控制台](docs/images/qt-console.png)

![Qt 演示中心](docs/images/qt-demo-lab.png)

![Qt 服务管理](docs/images/qt-server.png)

![Qt 集群路由](docs/images/qt-cluster.png)

![Qt 运行指标](docs/images/qt-stats.png)

![Qt 压测](docs/images/qt-benchmark.png)

## 性能压测

```bash
scripts/benchmark.sh
```

当前开发机一次样本，环境与完整数据见 [docs/benchmark-report.md](docs/benchmark-report.md)：

```text
io_threads  cache_shards  value_size_bytes  command  requests/sec  p50 ms  p95 ms  p99 ms  max ms
4           16            64                SET      29922.20      1.551   2.879   3.831   8.631
4           16            64                GET      29455.08      1.583   2.823   3.671   6.791
4           16            1024              SET      27540.62      1.687   3.055   4.055   13.871
4           16            1024              GET      28344.67      1.663   2.959   3.847   8.031
```

可靠性演示：

```bash
scripts/recovery_soak.sh
```

测试和压测说明见 [docs/testing-benchmark.md](docs/testing-benchmark.md)。

## 文档导航

- [使用与部署](docs/usage.md)
- [平台支持](docs/platform.md)
- [架构与持久化](docs/architecture.md)
- [并发模型与安全性](docs/concurrency.md)
- [复制机制](docs/replication.md)
- [Cluster 模式](docs/cluster.md)
- [测试与压测](docs/testing-benchmark.md)
- [压测报告](docs/benchmark-report.md)
- [面试讲解指南](docs/interview-guide.md)

## 当前限制

- 聚焦 String/KV 缓存命令，不实现 List/Hash/Set/ZSet 等完整 Redis 数据结构。
- ACL 支持用户启停、角色、命令白名单/黑名单和 key 前缀限制，但不兼容完整 Redis ACL 语法。
- 没有 TLS，公网部署需要网关、内网隔离或安全组限制。
- 原生服务端目标平台是 Linux；Windows/macOS 推荐通过 Docker 或远程 Linux 服务端配合 Qt Console 使用。
- Replication 支持启动全量同步、轻量 backlog 增量同步、后续异步转发和手动 failover takeover，不提供完整 Redis PSYNC2、复制积压缓冲持久化和自动选主。
- Cluster 模式面向手动运维和演示场景，支持拓扑维护、slot 迁移和手动 takeover，不提供完整 Redis Cluster gossip、投票和一致性协议。
