# 使用与部署

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
cmake -S . -B build-qt \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINIREDIS_BUILD_QT_CONSOLE=ON \
  -DMINIREDIS_ENABLE_INTEGRATION_TESTS=OFF \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++

cmake --build build-qt -j
./build-qt/tools/qt_console/miniredis_qt_console
```

Qt Console 可以作为主要演示入口：顶部菜单和工具栏负责连接、运行、服务、集群、监控、自动验证和压测入口；左侧资源管理器展示当前连接、常用 KV 命令、诊断命令、集群命令和 ACL 命令；中间命令工作区提供多行编辑、模板、历史记录、选中/当前行执行、全部执行和统一输出；服务管理、演示中心、集群路由、运行指标、诊断工具、Prometheus 指标和压测能力都通过弹窗打开，主界面保持简洁。

持久化演示推荐流程：进入服务管理，勾选 `AOF` 后启动服务，点击持久化演示写入普通 key 和 TTL key，点击重写 AOF 压缩增量日志，再重启服务并在命令工作区查询 `GET persist:plain`、`TTL persist:plain`、`INFO persistence`。

如果只想演示项目能力，可以直接使用演示中心：

- `Run Replication Demo`：启动 master/replica，展示 backlog/PSYNC offset、replica 断开期间写入和重连后的增量恢复。
- `Run AOF Recovery Demo`：启动带 AOF 的单节点，写入数据、触发 rewrite、重启并验证恢复。
- `Run Cluster Demo`：启动三节点集群，展示 slots/nodes，并停止一个节点观察 pfail/fail。
- `Smoke 测试`：运行 `tests/integration/smoke.sh`，覆盖 RESP、AUTH、TTL、stats、cluster 等集成场景。
- `Recovery/Soak`：运行 `scripts/recovery_soak.sh`，覆盖持久化恢复、坏尾/重启和短时稳定性验证。
- `Replica 脚本`：运行 `scripts/replica_demo.sh smoke`，验证主从复制和只读副本行为。
- `自动故障转移`：运行 `scripts/failover_demo.sh smoke`，验证多数派选主、旧主降级、数据追平和写隔离。
- `Cluster 脚本`：运行 `scripts/cluster_demo.sh smoke`，验证 slot range、cluster nodes、slots 和路由行为。

压测入口支持两种模式：运行 SET/GET 直接调用 `redis-benchmark` 测试当前连接服务，运行矩阵压测调用 `scripts/benchmark.sh`，按 `1 / 当前值` 对比 `io_threads` 和 `cache_shards` 组合。

## 本地运行

默认只监听本机地址，避免误暴露到公网：

```bash
./build/miniredis
```

使用配置文件：

```bash
./build/miniredis --config config/miniredis.conf
```

配置优先级为：默认值 < 配置文件 < 环境变量 < 命令行参数。

使用 `redis-cli`：

```bash
redis-cli -p 6366 PING
redis-cli -p 6366 SET foo bar
redis-cli -p 6366 SETNX foo first
redis-cli -p 6366 SET temp value EX 30
redis-cli -p 6366 EXPIRE foo 60
redis-cli -p 6366 TTL temp
redis-cli -p 6366 PEXPIRE foo 1500
redis-cli -p 6366 PTTL foo
redis-cli -p 6366 PERSIST foo
redis-cli -p 6366 GET foo
redis-cli -p 6366 GETEX foo EX 60
redis-cli -p 6366 GETDEL foo
redis-cli -p 6366 MGET foo temp missing
redis-cli -p 6366 APPEND foo !
redis-cli -p 6366 STRLEN foo
redis-cli -p 6366 TYPE foo
redis-cli -p 6366 INCR counter
redis-cli -p 6366 INCRBY counter 10
redis-cli -p 6366 INFO memory
redis-cli -p 6366 SLOWLOG LEN
redis-cli -p 6366 SLOWLOG GET 10
```

启用认证：

```bash
./build/miniredis --requirepass change-me
redis-cli -p 6366 PING
redis-cli -p 6366 -a change-me SET foo bar
```

启用轻量 ACL：

```bash
./build/miniredis \
  --acl-user admin:adminpass:admin \
  --acl-user 'app password=apppass role=readwrite commands=ping,get,set,setnx,mget,getdel,getex,append,strlen,type,incr,decr,incrby,decrby,del,exists,expire,pexpire,persist,ttl,pttl,info,slowlog,command,acl,bgrewriteaof keys=app:* enabled=true' \
  --acl-user auditor:auditpass:readonly

redis-cli -p 6366 AUTH app apppass
redis-cli -p 6366 SET app:foo bar
redis-cli -p 6366 ACL WHOAMI
redis-cli -p 6366 AUTH auditor auditpass
redis-cli -p 6366 GET app:foo
redis-cli -p 6366 AUTH admin adminpass
redis-cli -p 6366 ACL LIST
redis-cli -p 6366 ACL GETUSER app
```

配置文件也可以写成：

```text
user = admin password=adminpass role=admin
user = app password=apppass role=readwrite commands=ping,get,set,setnx,mget,getdel,getex,append,strlen,type,incr,decr,incrby,decrby,del,exists,expire,pexpire,persist,ttl,pttl,info,slowlog,command,acl,bgrewriteaof keys=app:* enabled=true
user = auditor password=auditpass role=readonly
```

`admin` 可执行所有命令并查看 `ACL LIST/GETUSER`；`readwrite` 可执行基础 KV 读写；`readonly` 只能执行读命令、基础信息查询和 `ACL WHOAMI`。`commands=` 可用逗号配置命令白名单，也支持 `-del` 这种显式拒绝；`keys=` 使用前缀匹配，例如 `app:*`。旧版 `--requirepass` / `AUTH password` 仍然兼容。

启用 AOF 增量日志：

```bash
./build/miniredis \
  --snapshot-file build/snapshot_6366.dat \
  --appendonly-file build/appendonly_6366.aof \
  --appendfsync everysec
```

启动时会先恢复 snapshot，再 replay AOF。`appendfsync=always` 数据更稳但写入更慢；`appendfsync=no` 依赖操作系统刷盘，吞吐更高但异常退出时风险更大。

## Docker

项目提供 Dockerfile 和 docker-compose 示例：

```bash
export MINIREDIS_REQUIREPASS='change-me'
docker compose up --build
```

默认映射：

```text
RESP:  127.0.0.1:6366
Stats: http://127.0.0.1:8080
AUTH:  ${MINIREDIS_REQUIREPASS:-change-me}
```

验证服务：

```bash
redis-cli -p 6366 -a "${MINIREDIS_REQUIREPASS:-change-me}" PING
curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/readyz
curl http://127.0.0.1:8080/stats
curl http://127.0.0.1:8080/metrics
redis-cli -p 6366 -a "${MINIREDIS_REQUIREPASS:-change-me}" INFO stats
```

快照数据默认保存在 Docker volume `miniredis-data` 中，避免容器重建时丢失。

## 配置说明

命令行参数：

```text
--bind ip                 RESP 监听地址，默认 127.0.0.1
--config path             加载 key=value 配置文件
--log-file path           追加写入结构化日志，默认输出到 stderr
--log-level level         debug、info、warn 或 error，默认 info
--port port               RESP 端口，默认 6366
--stats-bind ip           stats 监听地址，默认 127.0.0.1
--stats-port port         stats 端口，默认 8080
--snapshot-file path      快照文件路径，默认 snapshot_<port>.dat
--snapshot-interval sec   快照间隔，默认 20 秒
--appendonly-file path    启用 AOF 增量日志
--appendfsync policy      no、everysec 或 always，默认 everysec
--replicaof ip:port       以只读 replica 模式跟随指定 master
--replicas a:port,b:port  master 侧写命令转发的 replica 列表
--replication-backlog-size count  增量复制 backlog 条目上限，默认 10000
--replication-sync-interval-ms ms  replica 主动追平间隔，默认 1000
--replication-reconnect-delay-ms ms  master 后台复制重连间隔，默认 500
--automatic-failover      启用固定成员组自动故障转移
--failover-nodes nodes    至少三个投票节点，逗号分隔且包含当前节点
--failover-state-file path  epoch、投票和 leader 持久化文件
--failover-heartbeat-interval-ms ms  故障探测间隔，默认 1000
--failover-fail-threshold count  连续失败阈值，默认 3
--requirepass password    启用 AUTH
--acl-user spec 添加 ACL 用户，支持 user:pass:role，也支持 user password=pass role=readwrite commands=get,set,mget keys=app:* enabled=true，可重复传
--max-request-bytes bytes 单连接最大请求缓冲，默认 16777216
--max-key-bytes bytes     最大 key 长度，默认 4096
--max-value-bytes bytes   最大 value 长度，默认 16777216
--max-pipeline-commands count 单次读事件最多处理的 pipeline 命令数，默认 1024
--client-output-buffer-limit bytes 单连接待写响应缓冲上限，默认 33554432
--max-clients count       最大并发 RESP 客户端数，默认 10000
--io-threads count        客户端 IO worker reactor 数，默认 4
--cache-shards count      CacheStore 分片数，默认 16，用于降低 KV 锁竞争
--maxmemory bytes         最大缓存 payload 字节数，0 表示不限制
--eviction-policy policy  noeviction 或 lru，默认 noeviction
--slowlog-log-slower-than-us us  慢命令阈值，单位微秒，默认 10000，0 表示关闭
--slowlog-max-len count   最多保留多少条慢日志，默认 128
--cluster                 启用实验性集群路由
--enable-node-discovery   启用 MySQL 节点发现
--cluster-heartbeat sec   Cluster 节点心跳间隔，默认 2 秒
--cluster-fail-threshold count  连续失败多少次后标记 fail，默认 3
--cluster-config-file path  持久化并恢复 cluster slots / 节点状态
--node-addr ip:port       当前集群节点地址
--nodes a:port,b:port     初始集群节点列表
--mysql-host host         MySQL 节点发现地址
--mysql-user user         MySQL 用户
--mysql-pass password     MySQL 密码
--mysql-db db             MySQL 数据库
--mysql-port port         MySQL 端口
```

启用 AOF 后可以手动触发后台 rewrite 压缩日志：

```bash
redis-cli -p 6366 BGREWRITEAOF
```

环境变量：

```text
MINIREDIS_BIND
MINIREDIS_CONFIG
MINIREDIS_LOG_FILE
MINIREDIS_LOG_LEVEL
MINIREDIS_PORT
MINIREDIS_STATS_BIND
MINIREDIS_STATS_PORT
MINIREDIS_SNAPSHOT_FILE
MINIREDIS_SNAPSHOT_INTERVAL
MINIREDIS_APPENDONLY_FILE
MINIREDIS_APPENDFSYNC
MINIREDIS_REPLICAOF
MINIREDIS_REPLICAS
MINIREDIS_AUTOMATIC_FAILOVER
MINIREDIS_FAILOVER_NODES
MINIREDIS_FAILOVER_STATE_FILE
MINIREDIS_FAILOVER_HEARTBEAT_INTERVAL_MS
MINIREDIS_FAILOVER_FAIL_THRESHOLD
MINIREDIS_REQUIREPASS
MINIREDIS_MAX_REQUEST_BYTES
MINIREDIS_MAX_KEY_BYTES
MINIREDIS_MAX_VALUE_BYTES
MINIREDIS_MAX_PIPELINE_COMMANDS
MINIREDIS_CLIENT_OUTPUT_BUFFER_LIMIT
MINIREDIS_MAX_CLIENTS
MINIREDIS_IO_THREADS
MINIREDIS_CACHE_SHARDS
MINIREDIS_MAXMEMORY
MINIREDIS_EVICTION_POLICY
MINIREDIS_SLOWLOG_LOG_SLOWER_THAN_US
MINIREDIS_SLOWLOG_MAX_LEN
MINIREDIS_CLUSTER_HEARTBEAT_INTERVAL
MINIREDIS_CLUSTER_FAIL_THRESHOLD
MINIREDIS_CLUSTER_CONFIG_FILE
MINIREDIS_ACL_USERS
MINIREDIS_MYSQL_HOST
MINIREDIS_MYSQL_USER
MINIREDIS_MYSQL_PASS
MINIREDIS_MYSQL_DB
MINIREDIS_MYSQL_PORT
```

`MINIREDIS_ACL_USERS` 使用分号分隔多个用户，例如：

```bash
MINIREDIS_ACL_USERS='admin:adminpass:admin;app:apppass:readwrite;auditor:auditpass:readonly'
```

公网部署时必须显式设置 `--bind 0.0.0.0`，建议同时启用 `--requirepass` 或 ACL，并通过安全组或防火墙限制来源。

## 监控接口

```bash
curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/readyz
curl http://127.0.0.1:8080/stats
curl http://127.0.0.1:8080/metrics
```

`/healthz` 表示 HTTP 监控服务存活，`/readyz` 表示 RESP 服务已经 ready，可以接业务流量；`/stats` 返回 JSON，`/metrics` 返回 Prometheus text format。监控字段包含命令数、命中率、连接数、内存池、慢命令、资源保护、snapshot/AOF、replication offset，以及 failover quorum、可达节点、epoch、写隔离和选举次数。

健康检查示例：

```json
{"status":"ok"}
```

就绪检查示例：

```json
{"status":"ready"}
```

stats 示例：

```json
{
  "node_addr": "127.0.0.1:6366",
  "total_commands": 5,
  "get_hits": 1,
  "get_misses": 0,
  "hit_rate": 1.0000,
  "key_count": 1,
  "cache_shards": 16,
  "mem_pool_used_blocks": 1,
  "mem_pool_free_blocks": 1023,
  "connected_clients": 0,
  "total_connections": 8,
  "rejected_connections": 0,
  "latency_samples": 7,
  "avg_command_latency_us": 3,
  "max_command_latency_us": 18,
  "slowlog_len": 1,
  "slowlog_log_slower_than_us": 10000,
  "slowlog_max_len": 128,
  "ready": true,
  "io_threads": 4
}
```

## Systemd

systemd 示例位于：

```text
deploy/miniredis.service
```

生产部署推荐直接参考 [生产部署指南](production-deploy.md)。如果手动安装，可以使用下面的最小流程：

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin miniredis
sudo mkdir -p /opt/miniredis /var/lib/miniredis /var/log/miniredis /etc/miniredis
sudo cp build/miniredis /opt/miniredis/
sudo cp config/miniredis.prod.conf /etc/miniredis/miniredis.conf
sudo cp deploy/miniredis.service /etc/systemd/system/miniredis.service
sudo chown -R miniredis:miniredis /var/lib/miniredis
sudo chown -R miniredis:miniredis /var/log/miniredis
sudo install -m 600 deploy/miniredis.env.example /etc/miniredis/miniredis.env
sudoedit /etc/miniredis/miniredis.env
sudo systemctl daemon-reload
sudo systemctl enable --now miniredis
```
