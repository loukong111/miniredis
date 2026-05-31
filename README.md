# MiniRedis：基于 C++20 的轻量级内存缓存系统

## 项目简介

MiniRedis 是一个使用 C++20 实现的轻量级 Redis-like 内存缓存系统，主要用于学习和实践 C++ 后端开发中的网络编程、协程调度、内存管理、缓存持久化和简单集群分片等核心技术。

项目实现了 RESP 协议解析、基础 Redis 命令、`epoll` + C++20 协程网络模型、定长内存池、TTL 过期、MySQL 快照持久化、一致性哈希路由和 HTTP 监控接口，可通过 `redis-cli` 进行基础交互测试。

> 说明：本项目是学习型后端项目，重点在于实现核心机制和验证设计思路，并非生产级 Redis 替代品。

## 核心功能

- **RESP 协议解析**：支持 Redis Serialization Protocol 的基础解析与响应编码。
- **基础缓存命令**：支持 `SET`、`GET`、`DEL`、`EXISTS`、`PING` 等常用命令。
- **协程网络模型**：基于 `epoll` 与 C++20 协程封装非阻塞读写流程，减少回调嵌套。
- **定长内存池**：针对不超过 64B 的小对象使用固定块内存池复用分配，大对象走堆分配。
- **TTL 过期机制**：支持 key 的过期时间记录，并在访问或清理时过滤过期数据。
- **MySQL 快照持久化**：周期性将内存数据保存到 MySQL，服务启动时可从快照中恢复。
- **一致性哈希分片**：在集群模式下按 key 路由到目标节点，非本节点 key 返回 `MOVED` 重定向。
- **HTTP 监控接口**：提供 `/stats` 接口，查看命令数、命中情况、key 数量和内存池使用情况。
- **动态线程池**：用于处理后台快照任务，支持线程扩容和空闲回收。

## 技术栈

| 模块 | 实现 |
| --- | --- |
| 开发语言 | C++20 |
| 构建工具 | CMake |
| 网络模型 | Socket + epoll + C++20 coroutine |
| 协议解析 | RESP |
| 内存管理 | 64B 定长内存池 + 堆分配 |
| 并发控制 | mutex / shared_mutex / atomic |
| 持久化 | MySQL 快照 |
| 集群路由 | 一致性哈希 + MOVED 重定向 |
| 监控接口 | 原生 Socket HTTP Server |

## 项目结构

```text
miniredis/
├── CMakeLists.txt
├── main.cpp
├── include/
│   ├── cache_store.hpp           # 缓存存储结构
│   ├── consistent_hash.hpp       # 一致性哈希
│   ├── http_stats.hpp            # HTTP 统计接口
│   ├── memory_pool.hpp           # 定长内存池
│   ├── mysql_client.hpp          # MySQL 封装
│   ├── persistence_manager.hpp   # 快照持久化管理
│   ├── resp_parser.hpp           # RESP 协议解析
│   ├── scheduler.hpp             # 协程调度器
│   ├── stats.hpp                 # 运行统计
│   └── thread_pool.hpp           # 动态线程池
├── src/
│   ├── cache_store.cpp
│   ├── consistent_hash.cpp
│   ├── http_stats.cpp
│   ├── memory_pool.cpp
│   ├── mysql_client.cpp
│   ├── persistence_manager.cpp
│   ├── resp_parser.cpp
│   ├── scheduler.cpp
│   ├── stats.cpp
│   └── thread_pool.cpp
└── README.md
```

## 环境依赖

建议环境：

- Ubuntu 22.04 / 24.04 或 WSL2
- g++ 13+，需要支持 C++20 协程
- CMake 3.20+
- MySQL 8.0+
- `libmysqlclient-dev`
- `redis-tools`，用于安装 `redis-cli` 测试客户端

安装示例：

```bash
sudo apt update
sudo apt install -y g++-13 cmake mysql-server libmysqlclient-dev redis-tools
```

## 编译

```bash
git clone https://github.com/loukong111/miniredis.git
cd miniredis
mkdir -p build
cd build
cmake .. -DCMAKE_CXX_COMPILER=g++-13
make -j$(nproc)
```

编译完成后会生成：

```bash
./miniredis
```

## MySQL 初始化

源码中默认连接参数位于 `main.cpp`：

```cpp
MySQLClient mysql("127.0.0.1", "miniredis", "198407", "miniredis", 3306);
```

首次运行前需要创建数据库、用户和数据表。可根据本地环境修改用户名和密码。

```sql
CREATE DATABASE IF NOT EXISTS miniredis DEFAULT CHARSET utf8mb4;

CREATE USER IF NOT EXISTS 'miniredis'@'localhost' IDENTIFIED BY '198407';
GRANT ALL PRIVILEGES ON miniredis.* TO 'miniredis'@'localhost';
FLUSH PRIVILEGES;

USE miniredis;

CREATE TABLE IF NOT EXISTS cache_snapshot (
    cache_key VARCHAR(255) PRIMARY KEY,
    cache_value TEXT NOT NULL,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS cluster_nodes (
    node_addr VARCHAR(64) PRIMARY KEY,
    last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);
```

## 运行方式

### 1. 单机模式

```bash
./miniredis
```

单机模式默认监听 `6366` 端口。

使用 `redis-cli` 测试：

```bash
redis-cli -p 6366
```

示例：

```bash
127.0.0.1:6366> SET name alice
OK
127.0.0.1:6366> GET name
"alice"
127.0.0.1:6366> EXISTS name
(integer) 1
127.0.0.1:6366> DEL name
(integer) 1
```

### 2. 集群模式

打开三个终端，分别启动三个节点：

```bash
./miniredis --cluster --node-addr 127.0.0.1:6379 --nodes 127.0.0.1:6379,127.0.0.1:6380,127.0.0.1:6381
```

```bash
./miniredis --cluster --node-addr 127.0.0.1:6380 --nodes 127.0.0.1:6379,127.0.0.1:6380,127.0.0.1:6381
```

```bash
./miniredis --cluster --node-addr 127.0.0.1:6381 --nodes 127.0.0.1:6379,127.0.0.1:6380,127.0.0.1:6381
```

连接任意节点执行命令：

```bash
redis-cli -p 6379
```

如果 key 不属于当前节点，服务端会返回类似结果：

```text
-MOVED 0 127.0.0.1:6380
```

当前实现仅返回重定向信息，客户端需要根据 `MOVED` 提示重新访问目标节点。

## HTTP 监控接口

服务启动后会在 `8080` 端口暴露统计接口：

```bash
curl http://127.0.0.1:8080/stats
```

返回示例：

```json
{
  "node_addr": "127.0.0.1:6379",
  "total_commands": 42,
  "get_hits": 20,
  "get_misses": 5,
  "key_count": 17,
  "mem_pool_used_blocks": 17,
  "mem_pool_free_blocks": 1007
}
```

## 实现要点

### 1. 协程调度器

项目将文件描述符注册到 `epoll`，在读写事件就绪时恢复对应协程，使网络处理逻辑可以用接近同步的方式编写。

### 2. RESP 协议解析

通过 `RespDecoder` 对客户端输入进行增量解析，支持处理半包和多条命令连续到达的情况。

### 3. 内存池

缓存 value 小于等于 64B 时使用固定块内存池，降低频繁申请和释放小对象带来的开销；大对象使用堆分配，避免固定块浪费。

### 4. 快照持久化

`PersistenceManager` 后台定时获取缓存快照，并通过动态线程池提交保存任务，将数据写入 MySQL。服务启动时会尝试从 MySQL 加载已有快照。

### 5. 一致性哈希

集群模式下使用一致性哈希维护节点环，按 key 计算目标节点。若请求落到非目标节点，返回 `MOVED` 重定向。

## 当前限制

- 当前仅支持 String 类型及少量基础命令，未实现 Hash、List、Set、ZSet 等复杂数据结构。
- 快照持久化是周期性保存，不等价于 Redis AOF 或强一致持久化。
- 集群模式主要用于演示一致性哈希和重定向，未实现完整的数据迁移、主从复制和故障恢复。
- HTTP 监控接口为简化实现，未接入鉴权、指标采集系统或可视化面板。
- 当前未提供完整压测报告，后续可补充 QPS、延迟、P99 和内存占用数据。

## 后续优化方向

- 补充 `redis-benchmark` 或自写压测工具的测试结果。
- 支持更多 Redis 数据类型和命令。
- 增加 AOF 持久化或 WAL 日志。
- 实现节点间数据迁移和更完整的集群管理。
- 增加单元测试和集成测试。
- 补充 Docker Compose，一键启动 MySQL 和多节点测试环境。
