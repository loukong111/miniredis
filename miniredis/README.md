# MiniRedis

MiniRedis 是一个 C++20 实现的轻量级内存 KV 服务，使用 epoll + 协程处理 RESP 请求，支持 Redis 常用命令子集、快照持久化、HTTP 统计接口、认证和配置化启动。

项目目标不是替代 Redis，而是作为一个可阅读、可测试、可部署演示的 C++ 后端工程项目，重点展示网络 IO、协议解析、并发控制、持久化和基础运维能力。

## 已实现能力

- RESP 协议子集：`PING`、`AUTH`、`SET`、`GET`、`DEL`、`EXISTS`、`COMMAND`
- C++20 coroutine + epoll 调度器
- 非阻塞 socket 读写和响应缓冲
- `std::shared_mutex` 保护的内存 KV 存储
- 小 value 固定块内存池，大 value 自动使用堆内存
- 二进制快照持久化，支持空格、换行和二进制 value
- 快照临时文件写入、`fsync`、`rename` 原子替换
- SIGINT/SIGTERM 优雅退出，退出前保存最后一次快照
- HTTP `/stats` 指标接口，包含命令、连接数、内存池和命令延迟
- 默认本机绑定，可选 AUTH
- CTest 单元测试
- experimental 一致性哈希集群路由和 MySQL 节点发现

## 项目结构

代码按职责分层，主线是单机 KV 服务，加分线是 experimental 集群路由：

```text
include/miniredis/
├── core/          # KV 存储、内存池、线程池
├── net/           # epoll 调度器、RESP 编解码
├── persistence/   # 文件快照和持久化管理
├── metrics/       # stats 统计和 HTTP 暴露
├── cluster/       # 一致性哈希和可选 MySQL 节点发现
└── server/        # 配置解析、命令处理、服务启动编排

src/
├── core/
├── net/
├── persistence/
├── metrics/
├── cluster/
└── server/

tests/             # CTest 单元测试
deploy/            # systemd 等部署示例
scripts/           # benchmark 等工程脚本
```

`main.cpp` 只保留入口逻辑：解析配置并启动 `MiniRedisServer`。命令处理集中在 `server/command_handler`，服务生命周期集中在 `server/server`。

## 构建

依赖：

- Linux
- CMake 3.20+
- 支持 C++20 的 g++
- 可选：`libmysqlclient-dev`
- 可选：`redis-tools`，用于 `redis-cli` 和 `redis-benchmark`

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
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
redis-cli -p 6366 GET foo
```

启用认证：

```bash
./build/miniredis --requirepass secret
redis-cli -p 6366 PING
redis-cli -p 6366 -a secret SET foo bar
```

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
--cluster                 启用实验性集群路由
--enable-node-discovery   启用 MySQL 节点发现
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
MINIREDIS_MYSQL_HOST
MINIREDIS_MYSQL_USER
MINIREDIS_MYSQL_PASS
MINIREDIS_MYSQL_DB
MINIREDIS_MYSQL_PORT
```

公网部署时必须显式设置 `--bind 0.0.0.0`，并建议同时启用 `--requirepass`，再通过安全组或防火墙限制来源。

## 监控

```bash
curl http://127.0.0.1:8080/stats
```

示例：

```json
{
  "node_addr": "127.0.0.1:6366",
  "total_commands": 5,
  "get_hits": 1,
  "get_misses": 0,
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

## 持久化设计

快照文件使用二进制格式：

- magic header：`MINIREDIS_SNAPSHOT_V1`
- entry count
- 每条记录保存 `key_length`、`value_length`、key bytes、value bytes

保存流程：

1. 写入 `snapshot.tmp`
2. flush
3. `fsync(snapshot.tmp)`
4. `rename(snapshot.tmp, snapshot.dat)`

这样可以避免进程崩溃时把正式快照覆盖成半截文件。新版本仍兼容读取旧版 `key value` 文本快照，但下一次保存会升级为二进制格式。

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

集成测试脚本位于 `tests/integration/smoke.sh`，覆盖：

- 启动带 AUTH 的服务
- 验证未认证请求被拒绝
- 验证 `SET`、`GET`、`DEL`、`EXISTS`
- 查询 `/stats`，验证 key 数和内存池指标
- 停服后重启，验证快照恢复
- 启动单节点 cluster 模式
- 验证 `CLUSTER INFO`、`CLUSTER NODES`、`CLUSTER KEYSLOT`
- 验证 cluster 模式下基础 `SET/GET`

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

- 使用一致性哈希按 key 路由
- 非本节点 key 返回 `MOVED <slot> <node>`，其中 slot 按 Redis Cluster CRC16 hash slot 规则计算
- 支持 Redis hash tag，例如 `foo{bar}1` 和 `x{bar}2` 会得到相同 slot
- 支持 `CLUSTER INFO`、`CLUSTER NODES`、`CLUSTER KEYSLOT key`
- 可选 MySQL `cluster_nodes` 表做节点发现

单节点 cluster 冒烟：

```bash
./build/miniredis --cluster \
  --bind 127.0.0.1 \
  --node-addr 127.0.0.1:6366 \
  --nodes 127.0.0.1:6366

redis-cli -p 6366 CLUSTER INFO
redis-cli -p 6366 CLUSTER NODES
redis-cli -p 6366 CLUSTER KEYSLOT 'foo{bar}1'
```

返回示例：

```text
cluster_enabled:1
cluster_state:ok
cluster_known_nodes:1
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

## 已知限制

- 只实现 Redis 命令子集
- 没有 TLS，公网部署需要网关或内网隔离
- AUTH 是基础口令认证，不支持 ACL
- stats 仍是基础指标，尚未统计延迟分位数和连接数
- 集群模式是实验性能力
- 没有 AOF、主从复制和自动故障转移
