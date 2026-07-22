# Cluster 模式

集群能力定位为小规模内网 shard routing 与 HA 演示，目标是展示 Redis Cluster 风格的分片路由、重定向、拓扑维护、slot 接管和节点可观测能力。

## 功能范围

- 使用 Redis Cluster CRC16 规则计算 `key -> slot`
- 维护 16384 个固定 hash slot 到节点的映射表
- 实际路由链路为 `key -> slot -> node`
- 非本节点 key 返回 `MOVED <slot> <node>`；迁移中的 slot 返回临时 `ASK <slot> <node>`
- 支持 hash tag，例如 `foo{bar}1` 和 `x{bar}2` 会得到相同 slot
- 支持 `CLUSTER INFO`
- 支持 `CLUSTER NODES`
- 支持 `CLUSTER MYID`
- 支持 `CLUSTER KEYSLOT key`
- 支持 `CLUSTER COUNTKEYSINSLOT slot`
- 支持 `CLUSTER SLOTS`
- 支持 `CLUSTER SLOTMAP` 导出 epoch、节点状态、slot ranges 和迁移状态
- 支持 `CLUSTER MEET <node>` / `CLUSTER FORGET <node>` 手动维护拓扑
- 支持 `ASKING`，目标节点只有收到一次性 `ASKING` 后才接受 importing slot 的下一条 key 命令
- 支持 `CLUSTER SETSLOT <slot> NODE/MIGRATING/IMPORTING/STABLE`
- 支持 `CLUSTER MIGRATE <slot> <target-node>` 做简单 slot 数据迁移
- slot owner、slot 状态和节点健康状态变化会递增 `cluster_current_epoch`
- 后台维护线程定期 PING 其他节点，先标记 `master,pfail`，连续失败后标记 `master,fail`
- replica 节点支持 `CLUSTER FAILOVER TAKEOVER` 手动晋升为 master，并接管原 master 的 slots
- 固定 failover 节点组支持 epoch 持久化投票、多数派自动选主、失去 quorum 写隔离和旧主降级；cluster 模式下接管会原子更新原 master 的 slots
- 节点间通过拉取 `CLUSTER SLOTMAP` 同步拓扑；当远端 epoch 更高时加载远端 slot map
- 支持 `--cluster-config-file` 持久化节点列表、slot ranges、节点状态和 epoch
- 可选 MySQL `cluster_nodes` 表做节点发现

## 三节点演示

```bash
scripts/cluster_demo.sh smoke
scripts/cluster_demo.sh fail-smoke
scripts/cluster_demo.sh migrate-smoke
```

手动启动：

```bash
scripts/cluster_demo.sh start

redis-cli -p 6366 CLUSTER INFO
redis-cli -p 6366 CLUSTER NODES
redis-cli -p 6366 CLUSTER MYID
redis-cli -p 6366 CLUSTER SLOTS
redis-cli -p 6366 CLUSTER SLOTMAP
redis-cli -p 6366 CLUSTER MEET 127.0.0.1:6368
redis-cli -p 6366 CLUSTER FORGET 127.0.0.1:6368
redis-cli -p 6366 CLUSTER MIGRATE <slot> 127.0.0.1:6367

scripts/cluster_demo.sh stop
```

## 单节点冒烟测试

```bash
./build/miniredis --cluster \
  --bind 127.0.0.1 \
  --node-addr 127.0.0.1:6366 \
  --nodes 127.0.0.1:6366 \
  --cluster-config-file build/cluster_6366.conf

redis-cli -p 6366 CLUSTER INFO
redis-cli -p 6366 CLUSTER NODES
redis-cli -p 6366 CLUSTER MYID
redis-cli -p 6366 CLUSTER KEYSLOT 'foo{bar}1'
redis-cli -p 6366 CLUSTER COUNTKEYSINSLOT 5061
redis-cli -p 6366 CLUSTER SLOTS
redis-cli -p 6366 CLUSTER SLOTMAP
```

## Epoch 与 Slot Map 同步

每个节点维护一个 `cluster_current_epoch`。当 slot owner 变化、slot 进入 `migrating/importing/stable` 状态、节点被标记为 `healthy/pfail/fail` 时，epoch 会递增。

后台维护线程除了心跳探测外，还会向健康 peer 拉取 `CLUSTER SLOTMAP`。如果远端 epoch 大于本地 epoch，本地会加载远端拓扑并持久化到 `--cluster-config-file`。这不是完整 Redis Cluster gossip，但能展示“版本化拓扑 + 节点间配置传播”的核心思路。

## 节点拓扑管理

`CLUSTER MEET <node>` 用于把一个节点加入本地拓扑，并标记为 `healthy`。新节点不会自动接管 slot，需要后续通过 `CLUSTER SETSLOT` 或 `CLUSTER MIGRATE` 进行显式迁移：

```bash
redis-cli -p 6366 CLUSTER MEET 127.0.0.1:6368
redis-cli -p 6366 CLUSTER NODES
```

`CLUSTER FORGET <node>` 用于移除不再参与服务的节点。为了避免误删导致 slot 无主，只有当目标节点已经不拥有任何 slot 时才允许移除：

```bash
redis-cli -p 6366 CLUSTER FORGET 127.0.0.1:6368
```

`MEET/FORGET` 会递增 `cluster_current_epoch`，并在配置了 `--cluster-config-file` 时持久化到本地配置文件。

## Slot 迁移

`CLUSTER SETSLOT` 用于手动调整 slot 状态：

```bash
redis-cli -p 6366 CLUSTER SETSLOT 42 MIGRATING 127.0.0.1:6367
redis-cli -p 6367 CLUSTER SETSLOT 42 IMPORTING 127.0.0.1:6366
redis-cli -p 6366 CLUSTER SETSLOT 42 NODE 127.0.0.1:6367
redis-cli -p 6367 CLUSTER SETSLOT 42 NODE 127.0.0.1:6367
```

`CLUSTER MIGRATE` 是项目提供的简化迁移命令，会扫描当前节点属于该 slot 的 key，写入目标节点，成功后删除本地 key，并向集群内其他节点广播新的 slot owner：

```bash
redis-cli -p 6366 CLUSTER MIGRATE 42 127.0.0.1:6367
```

当前迁移实现适合演示和中小数据量维护，支持简化 `ASK/ASKING` 流程，但不提供 Redis Cluster 完整异步迁移和回滚。

迁移状态下的路由行为：

- 源节点 slot 为 `MIGRATING` 时，相关 key 命令返回 `ASK <slot> <target-node>`
- 目标节点 slot 为 `IMPORTING` 时，未发送 `ASKING` 的 key 命令仍返回 `MOVED`
- 客户端向目标节点发送 `ASKING` 后，下一条 importing slot 的 key 命令会被放行

返回示例：

```text
cluster_enabled:1
cluster_state:ok
cluster_slots_assigned:16384
cluster_known_nodes:1
cluster_failed_nodes:0
cluster_suspect_nodes:0
cluster_current_epoch:1
cluster_current_node:127.0.0.1:6366
```

## Cluster 配置

`--cluster-config-file` 用于保存 cluster 元信息。首次启动时如果文件不存在，会根据 `--nodes` 初始化固定 slot 映射并写入文件；后续重启会优先加载该文件，恢复节点列表、slot range、`healthy/suspect/fail` 状态和 epoch。

配置文件是项目自定义的文本格式，便于排查：

```text
MINIREDIS_CLUSTER_CONFIG_V1
epoch 1
nodes 2
node 127.0.0.1:6366 healthy
node 127.0.0.1:6367 suspect
slots 2
slot 0 8191 127.0.0.1:6366
slot 8192 16383 127.0.0.1:6367
```

## 手动故障切换

`CLUSTER FAILOVER TAKEOVER` 是项目提供的简化手动故障转移命令。它只能在配置了 `--replicaof <master-node>` 的 replica 上执行：

```bash
redis-cli -p 6367 CLUSTER FAILOVER TAKEOVER
redis-cli -p 6367 INFO replication
redis-cli -p 6367 CLUSTER NODES
```

执行后当前 replica 会在运行态晋升为 master，原 master 会被标记为 `master,fail`，原 master 拥有的 slots 会转移到当前节点。该手动命令会跳过投票、复制 offset 比较和多数派确认，因此启用自动故障转移时会禁用它；自动模式见下节。

## 自动故障切换

自动模式与手动 `TAKEOVER` 分开：节点使用 `--failover-nodes` 配置固定投票组，至少三个成员。master 连续探测失败后，健康副本按 replication offset 和节点地址确定候选者；候选者在新 epoch 获得多数票后晋升，并向其他节点发布 leader。失去多数派的 master 会关闭写入，避免少数分区继续接收数据。

```bash
scripts/failover_demo.sh smoke
```

epoch、每轮投票和 leader 会原子保存到 `--failover-state-file`，进程重启后不会在同一 epoch 重复投票。该模型适合固定三节点的小规模内网部署，不等同于 Redis Cluster gossip、Sentinel 或 Raft。

## 节点发现表

```sql
CREATE TABLE IF NOT EXISTS cluster_nodes (
    node_addr VARCHAR(50) PRIMARY KEY,
    last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);
```

## Qt 演示

Qt Console 可以一键启动 3 节点 cluster demo，并可停止其中一个节点演示 `master,pfail` 到 `master,fail`。演示中心提供自动故障转移验证入口；Cluster 页提供迁移、`SETSLOT` 和 `MIGRATE` 操作。收到 `MOVED <slot> <node>` 后，Qt 客户端可以自动切换到目标节点并重试上一条命令。

## 当前限制

当前 cluster 模式解决的是请求路由、重定向、节点可观测、拓扑维护、简单 slot 迁移和固定成员组故障接管，不提供：

- Redis Cluster 完整异步迁移和回滚
- 动态成员变更、自动副本补充和跨机房仲裁
- Redis Cluster gossip、Sentinel 兼容或完整共识协议
- Redis Cluster 完整协议兼容
