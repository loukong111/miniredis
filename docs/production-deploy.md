# 生产部署指南

MiniRedis 的服务端定位是 Linux 内网服务。生产部署建议只暴露给可信业务网段，不建议裸露到公网。

## 部署目标

推荐目录：

```text
/opt/miniredis/miniredis          服务端二进制
/etc/miniredis/miniredis.conf     非敏感配置
/etc/miniredis/miniredis.env      密码和环境变量，权限 600
/var/lib/miniredis/               snapshot、AOF、cluster 配置
/var/log/miniredis/               结构化日志
```

推荐默认策略：

- RESP 端口默认绑定 `127.0.0.1`，需要内网访问时改成服务器内网 IP。
- `/stats`、`/metrics` 默认绑定 `127.0.0.1`，由本机 Prometheus agent 或安全网关采集。
- 非本地 RESP 监听必须启用 `requirepass` 或 ACL 用户。
- 生产配置默认启用 AOF `everysec`，snapshot 作为周期性基线备份。
- 使用 systemd 管理进程，使用 logrotate 控制日志增长。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 一键安装

```bash
sudo deploy/install.sh
sudoedit /etc/miniredis/miniredis.env
sudo systemctl enable --now miniredis
```

`deploy/install.sh` 会安装二进制、生产配置、systemd unit 和 logrotate 配置。首次安装会创建 `/etc/miniredis/miniredis.env`，请至少修改：

```text
MINIREDIS_REQUIREPASS=change-me
```

如果要给业务服务使用独立 ACL 用户，可以写成：

```text
MINIREDIS_ACL_USERS=admin:adminpass:admin;app password=apppass role=readwrite commands=ping,get,set,setnx,mget,getdel,getex,append,strlen,type,incr,decr,incrby,decrby,del,exists,expire,pexpire,persist,ttl,pttl,info,slowlog,command,bgrewriteaof keys=app:* enabled=true
```

## 手动安装

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin miniredis
sudo mkdir -p /opt/miniredis /etc/miniredis /var/lib/miniredis /var/log/miniredis
sudo install -m 0755 build/miniredis /opt/miniredis/miniredis
sudo install -m 0644 config/miniredis.prod.conf /etc/miniredis/miniredis.conf
sudo install -m 0600 deploy/miniredis.env.example /etc/miniredis/miniredis.env
sudo install -m 0644 deploy/miniredis.service /etc/systemd/system/miniredis.service
sudo install -m 0644 deploy/logrotate/miniredis /etc/logrotate.d/miniredis
sudo chown -R miniredis:miniredis /var/lib/miniredis /var/log/miniredis
sudo systemctl daemon-reload
sudo systemctl enable --now miniredis
```

## 验证

```bash
systemctl status miniredis
journalctl -u miniredis -n 100 --no-pager
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" PING
curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/readyz
curl http://127.0.0.1:8080/metrics
```

如果 `redis-cli` 不在服务器上，可以先用本机回环验证 HTTP health，再从可信业务机器连接 RESP 端口。

## 内网开放

只建议绑定服务器内网 IP，例如：

```text
MINIREDIS_BIND=10.0.0.12
MINIREDIS_REQUIREPASS=replace-with-strong-password
```

同时在安全组或防火墙限制来源：

```bash
sudo ufw allow from 10.0.0.0/24 to any port 6366 proto tcp
sudo ufw deny 6366/tcp
```

监控端口建议仍保持 `127.0.0.1:8080`。如果 Prometheus 从远程采集，优先通过本机 agent、反向代理认证或专用管理网段开放。

## 升级与回滚

升级前备份当前二进制和数据目录：

```bash
sudo deploy/backup.sh
sudo cp /opt/miniredis/miniredis /opt/miniredis/miniredis.bak.$(date +%Y%m%d%H%M%S)
```

替换二进制：

```bash
sudo install -m 0755 build/miniredis /opt/miniredis/miniredis
sudo systemctl restart miniredis
curl http://127.0.0.1:8080/readyz
```

如果启动失败，将备份二进制复制回 `/opt/miniredis/miniredis` 后重启。数据目录不要随程序发布一起覆盖。

本地构建发布包：

```bash
scripts/package_release.sh
ls -lh dist/miniredis-linux-x86_64-*.tar.gz
```

## 持久化恢复

启动时恢复顺序：

1. 加载 snapshot。
2. replay AOF 增量日志。
3. 如果 AOF 尾部不完整，会忽略坏尾。
4. 如果 snapshot 损坏，会尝试回退到 `.bak`。

建议生产配置启用：

```text
snapshot_file = /var/lib/miniredis/snapshot.dat
snapshot_interval = 300
appendonly_file = /var/lib/miniredis/appendonly.aof
appendfsync = everysec
```

手动压缩 AOF：

```bash
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" BGREWRITEAOF
redis-cli -p 6366 -a "$MINIREDIS_REQUIREPASS" INFO persistence
```

## 日志和监控

日志文件：

```text
/var/log/miniredis/miniredis.log
```

logrotate 配置：

```text
deploy/logrotate/miniredis
```

核心监控接口：

```text
/healthz   HTTP 服务存活
/readyz    RESP 服务就绪
/stats     JSON 指标
/metrics   Prometheus text format
```

上线后至少关注：

- `connected_clients`、`rejected_connections`
- `avg_command_latency_us`、`p95/p99` 延迟
- `key_count`、`used_memory_bytes`、`maxmemory`
- `aof_rewrite_in_progress`、`aof_last_rewrite_status`
- `snapshot_last_status`
- `slowlog_len`

## 常见故障

端口占用：

```bash
ss -lntp | grep -E '6366|8080'
```

权限不足：

```bash
sudo chown -R miniredis:miniredis /var/lib/miniredis /var/log/miniredis
```

服务无法 ready：

```bash
journalctl -u miniredis -n 200 --no-pager
curl -v http://127.0.0.1:8080/readyz
```

非本地 bind 启动失败：

```text
RESP bind address is not loopback; configure requirepass or ACL users
```

这是安全保护。配置 `MINIREDIS_REQUIREPASS` 或 `MINIREDIS_ACL_USERS` 后再启动。

更完整的故障处理流程见 [运维 Runbook](ops-runbook.md)。

## 上线前检查

- Release 构建通过。
- `ctest` 通过。
- `tests/integration/smoke.sh` 通过。
- `scripts/recovery_soak.sh` 通过。
- `scripts/resource_failure_soak.sh` 通过。
- 已启用认证或 ACL。
- 数据目录和日志目录权限正确。
- AOF 和 snapshot 路径位于持久化磁盘。
- RESP 端口只向可信业务网段开放。
- 监控端口不直接暴露公网。
- 有二进制回滚方案和数据备份方案。
