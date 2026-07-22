# 复制机制

MiniRedis 提供轻量主从复制和小规模自动故障转移，用于内网缓存服务的读写角色、全量/增量同步、身份校验、失效选主和写入隔离。

## 设计模型

- master 通过 `--replicas a:port,b:port` 配置副本列表
- replica 通过 `--replicaof master:port` 进入只读副本模式
- master 每次进程启动生成新的 40 位 `master_replid`，用来标识当前复制生命周期
- replica 启动及运行期间携带本地 `master_replid + offset` 请求 `REPLPSYNC`；只有 ID 一致且 backlog 覆盖该 offset 时才拉取缺失写命令
- replication ID 不一致、backlog 不足或 offset 无效时，replica 会退回 `REPLFULLSYNC`，加载当前全量 key/value/TTL 快照、新 ID 和 master offset
- master 成功执行写命令后只追加本地 replication backlog 并发布最新 offset，客户端线程不等待副本网络 IO
- 每个 replica 由独立后台 worker 维护长连接，通过 `REPLACK <replid>` 校验身份并获取已应用 offset，再按序发送 `REPLAPPLY <replid> <offset> ...`
- replica 校验 offset 连续性：重复投递按幂等成功处理，出现缺口时返回 `REPLGAP`，随后由周期 `REPLPSYNC` 追平
- replica 会拒绝旧 master 连接发来的 `REPLACK/REPLAPPLY`；主节点重启后即使新旧 offset 数值相同，也必须先完成全量同步
- replica 拒绝普通客户端写命令，但接受 master 发来的 `REPLAPPLY`、`REPLSET`、`REPLDEL`、`REPLEXPIRE`
- replica 如果启用 AOF，会把复制过来的写入也落到本地 AOF；启动全量同步后会触发一次本地 AOF rewrite
- 副本状态使用临时文件、`fsync` 和 `rename` 原子保存到 `<snapshot-file>.repl.state`；旧版 `.repl.offset` 可读取，但因缺少 ID 会安全地强制全量同步一次
- cluster 模式下 replica 可通过 `CLUSTER FAILOVER TAKEOVER` 手动晋升并接管原 master slots
- 启用自动故障转移后，节点按固定成员列表探测 master；多数派确认失败后，offset 最大且地址排序最小的健康副本发起 election
- election 使用单调递增 epoch，节点将 `last_voted_epoch/voted_for/leader` 原子持久化，保证每个 epoch 最多投一票
- master 只有在可达节点达到 quorum 时才接受写入；失去多数派返回 `MASTERDOWN`，旧 master 发现更高 epoch leader 后自动降级、全量追平并拒绝普通写入

查看状态：

```bash
redis-cli -p 6366 INFO replication
redis-cli -p 6367 INFO replication
```

`INFO replication` 会展示 `master_replid`、master/replica offset、backlog 长度、真实在线副本数，以及每个副本的 `state/offset/lag/reconnects/errors`。`/stats` 和 `/metrics` 同时暴露连接副本数、待追平 offset、重连、错误和 backlog miss。

复制相关可调参数：

```text
--replication-backlog-size count       backlog 最大命令条数，默认 10000
--replication-sync-interval-ms ms      replica 主动追平间隔，默认 1000
--replication-reconnect-delay-ms ms    master 后台重连间隔，默认 500
--automatic-failover                   启用固定节点组自动故障转移
--failover-nodes a:port,b:port,c:port  投票成员列表，至少三个且必须包含当前节点
--failover-state-file path             epoch/投票/leader 状态文件，默认 <snapshot>.failover.state
--failover-heartbeat-interval-ms ms     节点探测间隔，默认 1000
--failover-fail-threshold count         连续失败阈值，默认 3
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

自动故障转移验收：

```bash
scripts/failover_demo.sh smoke
```

脚本启动一个 master 和两个 replica，验证初始复制、master 宕机、多数派选主、新主写入、旧主重启降级与追平，以及 stale master 写隔离。

生产式演示中三个节点必须使用相同的 `--requirepass`，该口令也用于节点间 `REPLFAILOVER` 和复制链路认证。若启用了 ACL 但没有共享 `requirepass`，服务会拒绝以自动故障转移模式启动。

手动故障接管仍可用于运维演示：

```bash
redis-cli -p 6367 CLUSTER FAILOVER TAKEOVER
redis-cli -p 6367 INFO replication
```

执行后该节点运行态变为 master。自动模式应优先使用多数派 election，`TAKEOVER` 保留给明确了解风险的人工操作。

## 当前限制

当前复制具备全量同步、长连接异步推送、replication ID + offset 校验、backlog 断线追平和固定成员组自动选主，适合小规模内网服务，不提供：

- Redis PSYNC2 完整协议兼容
- Redis 风格的 `replid2` 双 ID 历史和 failover lineage 续传
- 复制积压缓冲持久化
- 长时间断线后的无限增量补偿
- 成员动态变更、自动副本补充和跨机房仲裁
- Raft 日志复制、完整 Redis Sentinel 或 Redis Cluster gossip/投票兼容
