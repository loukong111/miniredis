# 部署与运维

本文集中说明 Linux 内网实例的安装、升级、备份恢复和排障。构建选项与配置字段见 [使用指南](usage.md)，复制与集群操作分别见 [复制机制](replication.md) 和 [Cluster 模式](cluster.md)。

## 目录与默认配置

| 路径 | 用途 |
| :--- | :--- |
| `/opt/miniredis/miniredis` | 服务端二进制 |
| `/etc/miniredis/miniredis.conf` | 非敏感配置 |
| `/etc/miniredis/miniredis.env` | 认证和环境变量，权限 600 |
| `/var/lib/miniredis/` | snapshot、AOF、复制及集群状态 |
| `/var/log/miniredis/` | 日志 |
| `/var/backups/miniredis/` | 默认备份目录 |

示例配置默认只监听本机。需要业务网段访问时，绑定服务器内网 IP 并限制来源；非回环 RESP 监听必须配置 `requirepass` 或 ACL。监控接口没有认证，应保持本机访问或通过受控采集链路读取。

`config/miniredis.prod.conf` 启用 AOF `everysec` 和周期快照。`maxmemory` 是 key/value 字节额度，容量规划还应计入容器、连接缓冲、内存池和线程等额外内存。

## 首次安装

在源码目录构建并验证：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

sudo deploy/install.sh
sudoedit /etc/miniredis/miniredis.env
sudo systemctl enable --now miniredis
```

启动前将环境文件中的 `MINIREDIS_REQUIREPASS=change-me` 替换为部署口令。ACL 用户语法见 [使用指南](usage.md#本地运行)。安装脚本会创建服务用户、数据和日志目录，并安装 systemd unit 与 logrotate 配置。

发布包可通过以下命令生成：

```bash
scripts/package_release.sh
```

解压后在包目录运行 `sudo deploy/install.sh`。发布包默认关闭 MySQL 节点发现；需要该能力时应单独构建并准备对应运行库。安装脚本会覆盖主配置文件，升级前应备份已有配置。

## 验证与内网访问

```bash
systemctl status miniredis
journalctl -u miniredis -n 100 --no-pager
curl -fsS http://127.0.0.1:8080/healthz
curl -fsS http://127.0.0.1:8080/readyz
REDISCLI_AUTH="$MINIREDIS_REQUIREPASS" redis-cli -p 6366 PING
```

这里的 `MINIREDIS_REQUIREPASS` 需在当前终端设置；systemd 的环境文件不会自动导入交互终端。`/healthz` 表示监控服务存活，`/readyz` 表示 RESP 服务已就绪。后者失败时，应先查启动日志、端口与持久化路径。

如需开放内网访问，在环境文件中设置：

```text
MINIREDIS_BIND=10.0.0.12
MINIREDIS_REQUIREPASS=replace-with-deployment-password
```

修改后重启服务，并在防火墙或安全组中仅允许业务网段访问 RESP 端口。监控端口建议继续监听 `127.0.0.1:8080`。

## 备份与恢复

为保证 snapshot、AOF 和复制状态来自一致的文件集合，备份前安排停机窗口：

```bash
sudo systemctl stop miniredis
sudo deploy/backup.sh
sudo systemctl start miniredis
```

备份脚本包含数据目录、配置、二进制、systemd unit 和 logrotate 配置，默认输出 `/var/backups/miniredis/miniredis-backup-<timestamp>.tar.gz`。可用 `sudo BACKUP_ROOT=/data/backups/miniredis deploy/backup.sh` 指定目录。运行中复制文件不能保证跨文件的一致恢复点。

恢复前检查备份内容并保留当前数据副本，再停止服务、解压可信备份：

```bash
sudo systemctl stop miniredis
sudo tar --absolute-names -xzf /var/backups/miniredis/miniredis-backup-YYYYmmddHHMMSS.tar.gz
sudo chown -R miniredis:miniredis /var/lib/miniredis /var/log/miniredis
sudo systemctl daemon-reload
sudo systemctl start miniredis
curl -fsS http://127.0.0.1:8080/readyz
```

归档使用绝对路径，恢复会覆盖对应文件。恢复后检查 `INFO persistence`、`INFO memory` 和业务 key/TTL；复制或集群实例还需核对 leader、offset 和 slot ownership。

## 升级与回滚

先在目标环境验证新版本，再安排停机、备份并替换二进制：

```bash
sudo systemctl stop miniredis
sudo deploy/backup.sh
sudo cp /opt/miniredis/miniredis /opt/miniredis/miniredis.previous
sudo install -m 0755 build/miniredis /opt/miniredis/miniredis
sudo systemctl start miniredis
curl -fsS http://127.0.0.1:8080/readyz
```

升级失败时停止服务，将 `miniredis.previous` 复制回正式路径，再启动并验证。数据目录独立于程序发布；如版本涉及文件格式变化，应先验证兼容性，必要时连同升级前的数据备份一起恢复。

## 持久化与监控

启动时先加载 snapshot；主快照损坏会标记为 `.bad` 并尝试 `.bak`，随后 replay AOF。AOF 的不完整或非法尾部会被截断，前面的完整记录仍可恢复。文件格式和切换过程见 [架构设计](architecture.md#持久化)。

手动重写 AOF：

```bash
REDISCLI_AUTH="$MINIREDIS_REQUIREPASS" redis-cli -p 6366 BGREWRITEAOF
REDISCLI_AUTH="$MINIREDIS_REQUIREPASS" redis-cli -p 6366 INFO persistence
```

| 入口 | 主要用途 |
| :--- | :--- |
| `/healthz`、`/readyz` | 存活与就绪 |
| `/stats`、`/metrics` | JSON 与 Prometheus 指标 |
| `INFO memory` | key/value 内存、额度、淘汰 |
| `INFO persistence` | snapshot 和 AOF rewrite 状态 |
| `INFO replication` | 角色、replid、offset、副本连通性与选举状态 |
| `SLOWLOG GET` | 慢命令诊断 |

日志位于 `/var/log/miniredis/miniredis.log`，也可通过 `journalctl -u miniredis` 查看。应持续关注拒绝连接、延迟分位数、内存与淘汰、snapshot/AOF 失败和复制积压。日志轮转配置见 `deploy/logrotate/miniredis`。

## 故障处理

### 启动或就绪失败

```bash
journalctl -u miniredis -n 200 --no-pager
systemctl cat miniredis
ss -lntp
```

检查端口冲突、配置值、数据和日志目录权限，以及非回环监听是否启用认证。端口冲突应调整 `port`/`stats_port` 或停止已确认的旧实例。路径权限可按以下方式修复：

```bash
sudo chown -R miniredis:miniredis /var/lib/miniredis /var/log/miniredis
```

### 认证或授权失败

检查环境文件是否符合预期、权限是否为 600，以及客户端是否在当前连接发送认证。不同的 `redis-cli` 进程不会共享 AUTH 状态：

```bash
REDISCLI_AUTH="$MINIREDIS_REQUIREPASS" redis-cli -p 6366 ACL WHOAMI
REDISCLI_AUTH=apppass redis-cli -p 6366 --user app GET app:foo
```

ACL 用户的角色、命令权限和 key 前缀权限必须同时满足；`commands=` 不会提升角色权限。

### 写入返回 OOM

检查 `INFO memory` 和 `/stats`。`noeviction` 在额度不足时拒绝写入，`lru` 在命中 shard 内淘汰。由于额度按 shard 划分，热点 shard 可能先满，即使全局统计尚未达到上限。应结合 key 分布、TTL 和淘汰率调整容量或实例划分。

### AOF 增长或 rewrite 失败

检查 `INFO persistence`、日志、可用磁盘空间、目录权限和 rewrite buffer 限制。rewrite 失败会保留旧 AOF，可在消除原因后重试 `BGREWRITEAOF`；启动时会清理残留 `.rewrite.tmp`。

### Snapshot 损坏

查看数据目录和启动日志，确认 `.bad` 与 `.bak`。有可用备份时自动回退；无可用快照而 AOF 可用时仍会尝试 replay AOF。文件组严重损坏时先停服、保留现场，再从已验证备份恢复。

### 写入返回 READONLY 或 MASTERDOWN

`READONLY` 表示当前实例为 replica；`MASTERDOWN` 表示自动故障转移组未达到写入所需 quorum。通过 `INFO replication` 核对当前 leader、成员连通性和 epoch，再将客户端连接到可写主节点。选举与恢复步骤见 [复制机制](replication.md)。

## 部署前验证

确认 Release 构建和 CTest 通过，运行恢复与资源故障验证，并在目标硬件上完成容量评估：

```bash
REQUESTS=100000 CLIENTS=50 scripts/recovery_soak.sh
REQUESTS=100000 CLIENTS=50 scripts/resource_failure_soak.sh
BENCH_MATRIX=1 IO_THREADS_LIST="1 4" CACHE_SHARDS_LIST="1 16" scripts/benchmark.sh
```

部署配置还应确认认证、网段限制、数据路径与权限、监控采集，以及经过演练的备份和回滚流程。脚本参数及数据解释见 [测试与压测](testing-benchmark.md)。
