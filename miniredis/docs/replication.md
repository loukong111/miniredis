# 复制机制

MiniRedis 提供简化主从复制，用于演示内网缓存服务的读写角色、启动全量同步、backlog 增量同步和写命令转发链路。

## 设计模型

- master 通过 `--replicas a:port,b:port` 配置副本列表
- replica 通过 `--replicaof master:port` 进入只读副本模式
- replica 启动及运行期间携带本地 offset 请求 `REPLPSYNC`，如果 master backlog 覆盖该 offset，则只拉取缺失写命令
- backlog 不足或 offset 无效时，replica 会退回 `REPLFULLSYNC`，加载当前全量 key/value/TTL 快照和 master offset
- master 成功执行写命令后只追加本地 replication backlog 并发布最新 offset，客户端线程不等待副本网络 IO
- 每个 replica 由独立后台 worker 维护长连接，通过 `REPLACK` 获取已应用 offset，再按序发送 `REPLAPPLY <offset> ...`
- replica 校验 offset 连续性：重复投递按幂等成功处理，出现缺口时返回 `REPLGAP`，随后由周期 `REPLPSYNC` 追平
- replica 拒绝普通客户端写命令，但接受 master 发来的 `REPLAPPLY`、`REPLSET`、`REPLDEL`、`REPLEXPIRE`
- replica 如果启用 AOF，会把复制过来的写入也落到本地 AOF；启动全量同步后会触发一次本地 AOF rewrite；复制 offset 会记录到 `<snapshot-file>.repl.offset`
- cluster 模式下 replica 可通过 `CLUSTER FAILOVER TAKEOVER` 手动晋升并接管原 master slots

查看状态：

```bash
redis-cli -p 6366 INFO replication
redis-cli -p 6367 INFO replication
```

`INFO replication` 会展示 master/replica offset、backlog 长度、真实在线副本数，以及每个副本的 `state/offset/lag/reconnects/errors`。`/stats` 和 `/metrics` 同时暴露连接副本数、待追平 offset、重连、错误和 backlog miss。

复制相关可调参数：

```text
--replication-backlog-size count       backlog 最大命令条数，默认 10000
--replication-sync-interval-ms ms      replica 主动追平间隔，默认 1000
--replication-reconnect-delay-ms ms    master 后台重连间隔，默认 500
```

## 演示方式

```bash
scripts/replica_demo.sh smoke
```

手动启动：

```bash
./build/miniredis \
  --port 6366 \
  --stats-port 18080 \
  --snapshot-file build/replica-demo/snapshot_master.dat \
  --replicas 127.0.0.1:6367

./build/miniredis \
  --port 6367 \
  --stats-port 18081 \
  --snapshot-file build/replica-demo/snapshot_replica.dat \
  --replicaof 127.0.0.1:6366
```

验证：

```bash
redis-cli -p 6366 SET repl:demo one
redis-cli -p 6367 GET repl:demo
redis-cli -p 6367 SET repl:demo blocked
```

第三条会返回 `READONLY`。

手动故障接管：

```bash
redis-cli -p 6367 CLUSTER FAILOVER TAKEOVER
redis-cli -p 6367 INFO replication
```

执行后该节点运行态变为 master；这是手动 takeover，不包含自动选主。

## 当前限制

当前复制具备全量同步、长连接异步推送、ACK/offset 校验和 backlog 断线追平，适合中小规模内网服务，不提供：

- Redis PSYNC2 完整协议兼容
- replication ID 切换和双 ID 历史
- 复制积压缓冲持久化
- 长时间断线后的无限增量补偿
- 自动故障转移和选主投票
- Sentinel / Redis Cluster replica 完整兼容
