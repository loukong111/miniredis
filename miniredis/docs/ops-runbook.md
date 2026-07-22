# 运维 Runbook

这份 Runbook 面向内网服务器上的 MiniRedis 单机或小规模集群部署，用来快速定位常见问题。

## 快速状态检查

```bash
systemctl status miniredis
journalctl -u miniredis -n 100 --no-pager
curl -fsS http://127.0.0.1:8080/healthz
curl -fsS http://127.0.0.1:8080/readyz
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" PING
```

如果 `/healthz` 正常但 `/readyz` 失败，优先查看 RESP 端口是否监听、持久化恢复是否失败、AOF/snapshot 路径权限是否正确。

## 服务启动失败

观察：

```bash
journalctl -u miniredis -n 200 --no-pager
systemctl cat miniredis
```

常见原因：

- 端口被占用。
- `/var/lib/miniredis` 或 `/var/log/miniredis` 权限不属于 `miniredis` 用户。
- 非本地 `bind` 未配置 `MINIREDIS_REQUIREPASS` 或 ACL 用户。
- 配置文件中端口、布尔值、`appendfsync`、`eviction_policy` 写错。

处理：

```bash
ss -lntp | grep -E '6366|8080'
sudo chown -R miniredis:miniredis /var/lib/miniredis /var/log/miniredis
sudoedit /etc/miniredis/miniredis.conf
sudoedit /etc/miniredis/miniredis.env
sudo systemctl restart miniredis
```

## 端口占用

观察：

```bash
ss -lntp | grep 6366
ss -lntp | grep 8080
```

处理：

- 如果是旧 MiniRedis 进程残留，先 `sudo systemctl stop miniredis`，确认后再停止残留进程。
- 如果端口被其他服务占用，修改 `/etc/miniredis/miniredis.conf` 中的 `port` 或 `stats_port`。

## 认证失败

观察：

```bash
redis-cli -p 6366 PING
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" PING
redis-cli -p 6366 AUTH app apppass
redis-cli -p 6366 ACL WHOAMI
```

处理：

- 检查 `/etc/miniredis/miniredis.env` 权限是否为 `600`。
- 检查 `MINIREDIS_REQUIREPASS` 或 `MINIREDIS_ACL_USERS` 是否符合预期。
- ACL 用户需要命令权限和 key 前缀权限都满足，例如业务 key 为 `app:*`。

## 内存达到上限

观察：

```bash
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" INFO memory
curl -fsS http://127.0.0.1:8080/stats
```

关注：

- `used_memory`
- `maxmemory`
- `maxmemory_policy`
- `evicted_keys`

处理：

- 如果 `noeviction` 返回 OOM，考虑调大 `maxmemory` 或改为 `lru`。
- 如果 `lru` 下淘汰过多，说明容量不足或 TTL 设置不合理。
- 短期处理可以降低写入量或清理业务前缀 key；长期处理应扩容或拆分实例。

## AOF 文件增长过快

观察：

```bash
ls -lh /var/lib/miniredis/appendonly.aof
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" INFO persistence
```

处理：

```bash
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" BGREWRITEAOF
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" INFO persistence
```

如果 `aof_rewrite_last_status` 为失败状态，查看日志：

```bash
journalctl -u miniredis -n 200 --no-pager
tail -n 200 /var/log/miniredis/miniredis.log
```

常见原因是磁盘空间不足、目录权限错误、rewrite buffer 超限。

## Snapshot 损坏或恢复异常

观察：

```bash
ls -lh /var/lib/miniredis
journalctl -u miniredis -n 200 --no-pager
```

MiniRedis 会将损坏 snapshot 标记为 `.bad`，并尝试加载 `.bak`。

处理：

- 如果 `.bak` 可用，服务会自动回退。
- 如果 snapshot 和 `.bak` 都不可用，但 AOF 可用，启动后仍会 replay AOF。
- 如果数据目录严重损坏，先停止服务，再从备份恢复。

## 备份

推荐使用脚本：

```bash
sudo deploy/backup.sh
```

默认输出：

```text
/var/backups/miniredis/miniredis-backup-<timestamp>.tar.gz
```

也可以指定输出目录：

```bash
sudo BACKUP_ROOT=/data/backups/miniredis deploy/backup.sh
```

## 恢复

恢复前先停止服务：

```bash
sudo systemctl stop miniredis
sudo tar --absolute-names -xzf /var/backups/miniredis/miniredis-backup-YYYYmmddHHMMSS.tar.gz
sudo chown -R miniredis:miniredis /var/lib/miniredis /var/log/miniredis
sudo systemctl start miniredis
curl -fsS http://127.0.0.1:8080/readyz
```

恢复后建议检查：

```bash
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" INFO persistence
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" INFO memory
```

## 升级失败回滚

升级前：

```bash
sudo deploy/backup.sh
sudo cp /opt/miniredis/miniredis /opt/miniredis/miniredis.bak.$(date +%Y%m%d%H%M%S)
```

回滚：

```bash
sudo systemctl stop miniredis
sudo cp /opt/miniredis/miniredis.bak.<timestamp> /opt/miniredis/miniredis
sudo systemctl start miniredis
curl -fsS http://127.0.0.1:8080/readyz
```

数据目录不要随版本发布包覆盖。

## 上线前压测

```bash
ctest --test-dir build --output-on-failure
REQUESTS=100000 CLIENTS=50 scripts/recovery_soak.sh
REQUESTS=100000 CLIENTS=50 scripts/resource_failure_soak.sh
BENCH_MATRIX=1 IO_THREADS_LIST="1 4" CACHE_SHARDS_LIST="1 16" scripts/benchmark.sh
```

关注：

- p95/p99/max 延迟是否稳定。
- RSS 和 `used_memory` 是否符合预期。
- `rejected_connections` 是否为 0。
- `evicted_keys` 是否符合容量规划。
- AOF rewrite 和 snapshot 最近状态是否正常。
