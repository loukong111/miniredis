# MiniRedis - 分布式内存缓存系统

## 项目简介

MiniRedis 是一个基于 C++20 实现的轻量级分布式内存缓存系统，支持 Redis 协议（RESP）、集群分片、数据持久化、协程网络模型和动态线程池。该项目是作为实习项目开发的，覆盖了高性能缓存系统的核心设计要点。

## 主要特性

- **C++20 协程 + epoll**：异步非阻塞网络 IO，高并发下资源占用低，代码可读性高。
- **动态线程池**：自动根据任务负载调整线程数（min=4, max=16），节省系统资源。
- **定长内存池**：针对 ≤64 字节的小对象优化分配效率，减少内存碎片。
- **RESP 协议**：完整支持 Redis 序列化协议，可与标准 `redis-cli` 交互。
- **一致性哈希集群**：支持节点动态添加/删除，请求按 key 自动路由，返回 `MOVED` 重定向。
- **MySQL 持久化**：周期快照保存到 MySQL 表，启动时自动加载，保证数据不丢失。
- **HTTP 统计接口**：每个节点暴露 `/stats` 端点，返回 JSON 格式的实时监控数据。

## 技术栈

| 模块          | 技术实现                                      |
| ------------- | --------------------------------------------- |
| 构建工具      | CMake 3.20+                                   |
| 编译器        | g++-13 (需支持 C++20)                         |
| 网络模型      | epoll + 协程（C++20 coroutines）              |
| 内存管理      | 定长内存池（64B 块）                          |
| 并发控制      | std::mutex, std::shared_mutex, std::atomic    |
| 持久化        | MySQL 8.0+ (libmysqlclient)                  |
| 监控接口      | 原生 Socket HTTP 服务 + JSON 输出            |
| 集群分片      | 一致性哈希（虚拟节点数可配置）                |

## 编译与运行

### 环境依赖

- Ubuntu 22.04 / 24.04 或 WSL2
- g++-13 或更高版本
- CMake 3.20+
- libmysqlclient-dev
- （可选）Qt6 开发包（仅监控端需要，核心功能不需要）

安装依赖：
```bash
sudo apt update
sudo apt install g++-13 cmake libmysqlclient-dev



编译
bash
git clone <your-repo-url> miniredis
cd miniredis
mkdir build && cd build
cmake .. -DCMAKE_CXX_COMPILER=g++-13
make -j$(nproc)

运行模式
1. 单机模式（默认）
bash
./miniredis
此时监听 6379 端口，可直接用 redis-cli -p 6379 连接。

2. 集群模式（三个节点示例）
打开三个终端，分别执行：
节点 A (端口 6379)：
bash
./miniredis --cluster --node-addr 127.0.0.1:6379 --nodes 127.0.0.1:6379,127.0.0.1:6380,127.0.0.1:6381

节点 B (端口 6380)：
bash
./miniredis --cluster --node-addr 127.0.0.1:6380 --nodes 127.0.0.1:6379,127.0.0.1:6380,127.0.0.1:6381

节点 C (端口 6381)
bash
./miniredis --cluster --node-addr 127.0.0.1:6381 --nodes 127.0.0.1:6379,127.0.0.1:6380,127.0.0.1:6381
使用 redis-cli 连接任一节点，执行 SET key value，如果 key 不属于当前节点，会返回 -MOVED 0 target_addr。

持久化配置
在 main.cpp 中可修改 MySQL 连接参数（默认用户 miniredis，空密码，数据库 miniredis）。首次运行前需手动创建数据库和表：
sql
CREATE DATABASE IF NOT EXISTS miniredis;
USE miniredis;
CREATE TABLE IF NOT EXISTS cache_snapshot (
    cache_key VARCHAR(255) PRIMARY KEY,
    cache_value TEXT NOT NULL,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);
持久化间隔默认 20 秒，可在 PersistenceManager_manager.hpp里的PersistenceManager 构造时调整。

HTTP 监控接口
每个节点启动后会在 8080 端口提供统计接口：
bash
curl http://127.0.0.1:8080/stats
返回示例：
json
{
  "node_addr": "127.0.0.1:6379",
  "total_commands": 42,
  "get_hits": 20,
  "get_misses": 5,
  "key_count": 17,
  "mem_pool_used_blocks": 17,
  "mem_pool_free_blocks": 1007
}



项目结构
text
miniredis/
├── CMakeLists.txt
├── main.cpp
├── include/               # 头文件
│   ├── cache_store.hpp
│   ├── consistent_hash.hpp
│   ├── http_stats.hpp
│   ├── memory_pool.hpp
│   ├── mysql_client.hpp
│   ├── persistence_manager.hpp
│   ├── resp_parser.hpp
│   ├── scheduler.hpp
│   ├── stats.hpp
│   └── thread_pool.hpp
├── src/                   # 源文件
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
└── third_party/           # 第三方依赖（空）
性能测试（示例）
内存池分配/释放：分配 100 万次 64B 对象，总耗时约 0.3 秒（测试环境：Intel i7-10750H）。
单节点 QPS：使用 redis-benchmark 测得 SET 约 8w/s，GET 约 10w/s（协程+epoll）。
集群水平扩展：添加节点后，一致性哈希重分布约 15% 的 key，业务几乎无感知。

未来扩展方向
支持主从复制和哨兵模式
增加 Lua 脚本支持
增加 AOF 持久化方式
支持更多数据类型（Hash、List 等）