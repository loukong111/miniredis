# MiniRedis
MiniRedis 是一个 C++20 实现的轻量级内存 KV 服务，使用 epoll + 协程处理 RESP 请求，支持 Redis 常用命令子集、快照持久化、HTTP 统计接口、认证和配置化启动。


## 已实现能力
- RESP 协议子集：`PING`、`AUTH`、`SET`、`GET`、`MGET`、`DEL`、`EXISTS`、`EXPIRE`、`TTL`、`COMMAND`
- `COMMAND` 返回当前支持命令的基础元信息，兼容 redis-cli 的命令探测场景
- C++20 coroutine + epoll 调度器，使用 ready queue 管理可运行协程
- 非阻塞 socket 读写和响应缓冲
- `std::shared_mutex` 保护的内存 KV 存储
- 小 value 固定块内存池，大 value 自动使用堆内存
- TTL 过期支持，访问时惰性删除，后台 cleanup 可批量释放过期 key
- 二进制快照持久化，支持空格、换行和二进制 value
- 快照临时文件写入、`fsync`、`rename` 原子替换，并限制异常快照 entry 数量
- 定时快照由动态线程池异步执行，避免快照落盘阻塞调度线程
- SIGINT/SIGTERM 优雅退出，退出前保存最后一次快照
- HTTP `/stats` 和 Prometheus `/metrics` 指标接口，包含命令、连接数、内存池和命令延迟
- HTTP `/healthz` 健康检查接口，便于容器和部署平台探活
- HTTP stats 接口支持完整请求读取、完整响应写回和请求头大小限制
- 支持 `--max-clients` 连接数上限，超限连接会被拒绝并计入统计
- 可选 Qt Console 控制台，支持服务启停、常用命令测试、Cluster 查询、MOVED 自动跟随、运行指标、Raw Command 和压测入口
- 默认本机绑定，可选 AUTH
- CTest 单元测试和 redis-cli/curl 集成冒烟测试
- experimental Redis Cluster 风格 slot 路由和 MySQL 节点发现

## 项目结构
代码按职责分层，主线是单机 KV 服务，加分线是 experimental 集群路由：
```text
include/miniredis/
├── core/          # KV 存储、内存池、线程池
├── net/           # epoll 调度器、RESP 编解码
├── persistence/   # 文件快照和持久化管理
├── metrics/       # stats 统计和 HTTP 暴露
├── cluster/       # Redis Cluster 风格 slot 路由表和可选 MySQL 节点发现
└── server/        # 配置解析、命令处理、服务启动编排
src/
├── core/
├── net/
├── persistence/
├── metrics/
├── cluster/
└── server/
tests/             # CTest 单元测试和集成冒烟测试
deploy/            # systemd 等部署示例
scripts/           # benchmark 等工程脚本
tools/qt_console/  # 可选 Qt 测试入口和状态展示工具
```

`main.cpp` 只保留入口逻辑：解析配置并启动 `MiniRedisServer`。命令处理集中在 `server/command_handler`，服务生命周期集中在 `server/server`。

## 构建
依赖：
- Linux
- CMake 3.20+
- 支持 C++20 的 g++
- 可选：`libmysqlclient-dev`
- 可选：`redis-tools`，用于 `redis-cli` 和 `redis-benchmark`
- 可选：`Qt6 Core/Widgets/Network`，用于构建 Qt Console

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

构建 Qt Console：
```bash
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=Release -DMINIREDIS_BUILD_QT_CONSOLE=ON
cmake --build build-qt -j
./build-qt/tools/qt_console/miniredis_qt_console
```

## 快速启动
默认只监听本机地址，避免误暴露到公网：
```bash
./build/miniredis
```

使用 `redis-cli`：
```bash
redis-cli -p 6366 PING
redis-cli -p 6366 SET foo bar
redis-cli -p 6366 SET temp value EX 30
redis-cli -p 6366 EXPIRE foo 60
redis-cli -p 6366 TTL temp
redis-cli -p 6366 GET foo
redis-cli -p 6366 MGET foo temp missing
```

启用认证：
```bash
./build/miniredis --requirepass secret
redis-cli -p 6366 PING
redis-cli -p 6366 -a secret SET foo bar
```

## Docker 启动
项目提供 Dockerfile 和 docker-compose 示例，用于快速拉起后端服务：
```bash
docker compose up --build
```

默认映射：
```text
RESP:  127.0.0.1:6366
Stats: http://127.0.0.1:8080
AUTH:  secret
```

验证服务：
```bash
redis-cli -p 6366 -a secret PING
curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/stats
curl http://127.0.0.1:8080/metrics
```

快照数据默认保存在 Docker volume `miniredis-data` 中，避免容器重建时丢失。

## 配置
命令行参数：
```text
--bind ip                 RESP 监听地址，默认 127.0.0.1
--port port               RESP 端口，默认 6366
--stats-bind ip           stats 监听地址，默认 127.0.0.1
--stats-port port         stats 端口，默认 8080
--snapshot-file path      快照文件路径，默认 snapshot_<port>.dat
--snapshot-interval sec   快照间隔，默认 20 秒
--requirepass password    启用 AUTH
--max-clients count       最大并发 RESP 客户端数，默认 10000
--cluster                 启用实验性集群路由
--enable-node-discovery   启用 MySQL 节点发现
--cluster-heartbeat sec   Cluster 节点心跳间隔，默认 2 秒
--cluster-fail-threshold count  连续失败多少次后标记 fail，默认 3
--node-addr ip:port       当前集群节点地址
--nodes a:port,b:port     初始集群节点列表
--mysql-host host         MySQL 节点发现地址
--mysql-user user         MySQL 用户
--mysql-pass password     MySQL 密码
--mysql-db db             MySQL 数据库
--mysql-port port         MySQL 端口
```

也支持环境变量：
```text
MINIREDIS_BIND
MINIREDIS_PORT
MINIREDIS_STATS_BIND
MINIREDIS_STATS_PORT
MINIREDIS_SNAPSHOT_FILE
MINIREDIS_SNAPSHOT_INTERVAL
MINIREDIS_REQUIREPASS
MINIREDIS_MAX_CLIENTS
MINIREDIS_MYSQL_HOST
MINIREDIS_MYSQL_USER
MINIREDIS_MYSQL_PASS
MINIREDIS_MYSQL_DB
MINIREDIS_MYSQL_PORT
MINIREDIS_CLUSTER_HEARTBEAT_INTERVAL
MINIREDIS_CLUSTER_FAIL_THRESHOLD
```

公网部署时必须显式设置 `--bind 0.0.0.0`，并建议同时启用 `--requirepass`，再通过安全组或防火墙限制来源。

## 监控
```bash
curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/stats
curl http://127.0.0.1:8080/metrics
```

stats HTTP server 是轻量同步实现，面向健康检查和低频监控；`/healthz` 返回健康状态，`/stats` 返回 JSON，`/metrics` 返回 Prometheus text format。请求读取会循环到 HTTP header 完整，响应写回会循环直到全部发送完成，并对过大的请求头返回 431。

健康检查示例：
```json
{"status":"ok"}
```

示例：
```json
{
  "node_addr": "127.0.0.1:6366",
  "total_commands": 5,
  "get_hits": 1,
  "get_misses": 0,
  "hit_rate": 1.0000,
  "key_count": 1,
  "mem_pool_used_blocks": 1,
  "mem_pool_free_blocks": 1023,
  "connected_clients": 0,
  "total_connections": 8,
  "rejected_connections": 0,
  "latency_samples": 7,
  "avg_command_latency_us": 3,
  "max_command_latency_us": 18
}
```

Prometheus 示例：
```text
miniredis_total_commands 123
miniredis_get_hits 80
miniredis_get_misses 20
miniredis_hit_rate 0.8000
miniredis_connected_clients 1
```

## Qt Console
`tools/qt_console` 提供一个可选的 Qt Widgets 控制台，用于在面试或本地调试时快速验证 MiniRedis，尽量减少对终端命令的依赖：
- 启动/停止 `miniredis` 服务进程，并配置端口、认证、快照、连接数和 cluster 参数
- 一键启动/停止 3 节点 cluster demo，并可停止其中一个节点演示 `master,fail`
- 连接 RESP 端口并自动执行 `AUTH`
- 测试 `PING`、`SET`、`GET`、`MGET`、`DEL`、`EXISTS`、`EXPIRE`、`TTL`、`COMMAND`
- 查询 `CLUSTER INFO`、`CLUSTER NODES`、`CLUSTER MYID`、`CLUSTER KEYSLOT`、`CLUSTER COUNTKEYSINSLOT`、`CLUSTER SLOTS`
- 收到 `MOVED <slot> <node>` 后可自动切换到目标节点并重试上一条命令
- 轮询 `/stats` 展示命令数、命中率、key 数、连接数和延迟
- 拉取 `/metrics` 展示 Prometheus text format 原始输出
- 通过 Raw Command 输入任意 RESP 命令，覆盖未做成按钮的测试场景
- 调用 `redis-benchmark` 对当前服务执行 SET/GET 压测

Qt Console 是项目演示工具，不参与服务端主流程；默认构建关闭，避免没有 Qt 环境时影响后端构建。

### Qt Console 截图
截图放在 `docs/images/` 下：
```text
docs/images/qt-connect.png
docs/images/qt-kv.png
docs/images/qt-cluster.png
docs/images/qt-stats.png
docs/images/benchmark-summary.png
```

如果需要完整展示 Qt 控制台，也建议补充 `qt-server.png`、`qt-raw-command.png` 和 `qt-benchmark.png`。

连接服务：
![Qt Connect](docs/images/qt-connect.png)
KV 操作：
![Qt KV](docs/images/qt-kv.png)
Cluster 查询：
![Qt Cluster](docs/images/qt-cluster.png)
运行监控：
![Qt Stats](docs/images/qt-stats.png)
压测结果：
![Benchmark Summary](docs/images/benchmark-summary.png)

## 持久化设计
快照文件使用二进制格式：
- magic header：`MINIREDIS_SNAPSHOT_V1`
- entry count
- 每条记录保存 `key_length`、`value_length`、key bytes、value bytes
- 加载时限制最大 entry 数，避免损坏快照触发巨大内存申请
- 单条 key/value 长度会做边界校验，异常文件会被拒绝加载

保存流程：
1. 写入 `snapshot.tmp`
2. flush
3. `fsync(snapshot.tmp)`
4. `rename(snapshot.tmp, snapshot.dat)`

这样可以避免进程崩溃时把正式快照覆盖成半截文件。定时快照会提交到动态线程池异步执行；如果上一轮快照仍在执行，本轮会跳过，避免并发写同一个快照文件。新版本仍兼容读取旧版 `key value` 文本快照，但下一次保存会升级为二进制格式。

## 部署示例
systemd 示例位于：
```text
deploy/miniredis.service
```

示例安装步骤：
```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin miniredis
sudo mkdir -p /opt/miniredis /var/lib/miniredis /etc/miniredis
sudo cp build/miniredis /opt/miniredis/
sudo cp deploy/miniredis.service /etc/systemd/system/miniredis.service
sudo chown -R miniredis:miniredis /var/lib/miniredis
sudo install -m 600 /dev/null /etc/miniredis/miniredis.env
echo 'MINIREDIS_REQUIREPASS=change-me' | sudo tee /etc/miniredis/miniredis.env
sudo systemctl daemon-reload
sudo systemctl enable --now miniredis
```

## 测试和验证
CMake 会注册单元测试；如果本机存在 `bash`、`redis-cli` 和 `curl`，还会注册集成冒烟测试：
```text
ctest --test-dir build --output-on-failure
1/2 Test #1: miniredis_unit_tests ............. Passed
2/2 Test #2: miniredis_integration_smoke ...... Passed
```

项目提供 GitHub Actions CI，位于 `.github/workflows/ci.yml`。CI 会在 Ubuntu 24.04 上安装依赖、执行 CMake Release 构建，并运行单元测试和 redis-cli/curl 集成冒烟测试。

集成测试脚本位于 `tests/integration/smoke.sh`，覆盖：
- 启动带 AUTH 的服务
- 验证未认证请求被拒绝
- 验证 `SET`、`SET EX`、`GET`、`MGET`、`DEL`、`EXISTS`、`EXPIRE`、`TTL`
- 查询 `/healthz`、`/stats` 和 `/metrics`，验证健康检查、key 数、内存池指标和命中率字段
- 停服后重启，验证快照恢复
- 启动单节点 cluster 模式
- 验证 `CLUSTER INFO`、`CLUSTER NODES`、`CLUSTER MYID`、`CLUSTER KEYSLOT`、`CLUSTER COUNTKEYSINSLOT`、`CLUSTER SLOTS`
- 验证 cluster 模式下基础 `SET/GET`

单元测试覆盖：
- RESP 半包解析和粘包解析
- CacheStore 基础读写、TTL 查询、TTL 惰性删除、TTL cleanup 释放内存池 block
- 二进制快照读写、异常 entry count 拒绝加载
- AUTH 未认证拒绝、错误密码拒绝、正确密码放行
- `SET EX`、`EXPIRE`、`TTL`、`MGET` 和 COMMAND 元信息响应
- Redis Cluster hash slot、固定 slot 映射表、MOVED 统计和 slot 路由一致性
- 动态线程池 stop 后拒绝继续提交任务

## 压测
压测脚本：
```bash
scripts/benchmark.sh
```

默认参数：
```text
requests: 100000
clients: 50
value sizes: 64B, 1KB, 10KB
output: build/benchmark-report.txt
```

当前环境一次样本结果：
```text
value_size_bytes  command  requests/sec  p50 ms  p95 ms  p99 ms  max ms
64                SET      30358.23      1.479   2.855   3.887   18.655
64                GET      31555.70      1.391   2.751   3.519   6.111
1024              SET      30266.35      1.487   2.911   3.775   6.647
1024              GET      28473.80      1.543   3.031   4.191   37.215
10240             SET      16123.83      2.983   4.383   5.295   25.743
10240             GET      21092.60      2.127   4.231   6.143   15.559
```

这个结果只作为当前开发机样本，不代表生产性能。延迟展示使用 p50/p95/p99/max，避免只看中位数掩盖尾延迟。正式评估建议固定 CPU 频率、独占 CPU、跑多轮取均值，并补充 Redis 基线对比。

## 集群模式
集群能力目前是 experimental shard routing：
- 使用 Redis Cluster CRC16 规则计算 `key -> slot`
- 维护 16384 个固定 hash slot 到节点的映射表，实际路由链路为 `key -> slot -> node`
- 非本节点 key 返回 `MOVED <slot> <node>`，其中 slot 与实际路由使用同一个计算结果
- 支持 Redis hash tag，例如 `foo{bar}1` 和 `x{bar}2` 会得到相同 slot，并路由到同一个节点
- 支持 `CLUSTER INFO`、`CLUSTER NODES`、`CLUSTER MYID`、`CLUSTER KEYSLOT key`、`CLUSTER COUNTKEYSINSLOT slot`、`CLUSTER SLOTS`，`CLUSTER NODES` 会展示节点负责的 slot 范围
- `CLUSTER INFO` 会展示 `cluster_slots_assigned` 和本进程 slot map 版本 `cluster_current_epoch`
- 后台维护线程会定期 PING 其他节点，连续失败后在 `CLUSTER NODES` 中标记 `master,fail`，并在 `CLUSTER INFO` 中更新 `cluster_failed_nodes`
- 可选 MySQL `cluster_nodes` 表做节点发现

三节点 cluster demo：
```bash
scripts/cluster_demo.sh smoke
scripts/cluster_demo.sh fail-smoke

scripts/cluster_demo.sh start

redis-cli -p 6366 CLUSTER INFO
redis-cli -p 6366 CLUSTER NODES
redis-cli -p 6366 CLUSTER MYID
redis-cli -p 6366 CLUSTER SLOTS

scripts/cluster_demo.sh stop
```

单节点 cluster 冒烟：
```bash
./build/miniredis --cluster \
  --bind 127.0.0.1 \
  --node-addr 127.0.0.1:6366 \
  --nodes 127.0.0.1:6366

redis-cli -p 6366 CLUSTER INFO
redis-cli -p 6366 CLUSTER NODES
redis-cli -p 6366 CLUSTER MYID
redis-cli -p 6366 CLUSTER KEYSLOT 'foo{bar}1'
redis-cli -p 6366 CLUSTER COUNTKEYSINSLOT 5061
redis-cli -p 6366 CLUSTER SLOTS
```

返回示例：
```text
cluster_enabled:1
cluster_state:ok
cluster_slots_assigned:16384
cluster_known_nodes:1
cluster_failed_nodes:0
cluster_current_epoch:1
cluster_current_node:127.0.0.1:6366
```

表结构：
```sql
CREATE TABLE IF NOT EXISTS cluster_nodes (
    node_addr VARCHAR(50) PRIMARY KEY,
    last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);
```

三节点启动示例：
```bash
./build/miniredis --cluster \
  --bind 127.0.0.1 \
  --node-addr 127.0.0.1:6366 \
  --nodes 127.0.0.1:6366,127.0.0.1:6367,127.0.0.1:6368
```

注意：当前集群模式解决的是请求路由、重定向、节点可观测和可选节点发现，不提供数据迁移、副本、故障转移，也不宣称完整兼容 Redis Cluster。
