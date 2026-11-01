import { spawn, type ChildProcess } from "node:child_process";
import { randomUUID } from "node:crypto";
import { access, mkdir, stat } from "node:fs/promises";
import { constants } from "node:fs";
import net from "node:net";
import { isAbsolute, join, resolve } from "node:path";
import { StringDecoder } from "node:string_decoder";
import { setTimeout as delay } from "node:timers/promises";
import type {
  DesktopEvent,
  LocalConfig,
  LocalNode,
  LogEntry,
  TaskInput,
  TaskKind,
  TaskState,
} from "../shared/contracts";
import { asError, RespConnection } from "./redis";
import { integer } from "./workspace";

export class LogBook {
  private readonly entries: LogEntry[] = [];
  constructor(private readonly emit: (event: DesktopEvent) => void) {}
  add(source: string, text: string, secrets: string[] = []): void {
    for (const secret of secrets.filter(Boolean))
      text = text.split(secret).join("••••••");
    const entry = {
      id: randomUUID(),
      source,
      text: text.slice(0, 16_384),
      timestamp: Date.now(),
    };
    this.entries.push(entry);
    if (this.entries.length > 500) this.entries.shift();
    this.emit({ type: "log", entry });
  }
  list(): LogEntry[] {
    return [...this.entries];
  }
  pipe(child: ChildProcess, source: string, secrets: string[] = []): void {
    for (const stream of [child.stdout, child.stderr]) {
      if (!stream) continue;
      const decoder = new StringDecoder("utf8");
      let pending = "";
      stream.on("data", (chunk: Buffer) => {
        pending += decoder.write(chunk);
        const lines = pending.split(/\r?\n/);
        pending = lines.pop() ?? "";
        for (const line of lines) if (line) this.add(source, line, secrets);
        if (pending.length > 16_384) {
          this.add(source, pending, secrets);
          pending = "";
        }
      });
      stream.on("end", () => {
        pending += decoder.end();
        if (pending) this.add(source, pending, secrets);
      });
    }
  }
}

function cleanEnvironment(): NodeJS.ProcessEnv {
  return Object.fromEntries(
    Object.entries(process.env).filter(
      ([name]) => !name.startsWith("MINIREDIS_"),
    ),
  );
}

async function validateBinary(binary: string): Promise<string> {
  if (typeof binary !== "string" || !binary.trim())
    throw new Error("请选择 MiniRedis 服务端程序");
  const path = resolve(binary);
  const info = await stat(path).catch(() => null);
  if (!info?.isFile()) throw new Error("服务端程序不存在");
  await access(path, constants.X_OK).catch(() => {
    throw new Error("服务端程序没有执行权限");
  });
  return path;
}

function groupSignal(child: ChildProcess, signal: NodeJS.Signals): void {
  if (!child.pid) return;
  try {
    if (process.platform !== "win32") process.kill(-child.pid, signal);
    else child.kill(signal);
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== "ESRCH") throw error;
  }
}

function groupAlive(child: ChildProcess): boolean {
  if (!child.pid) return false;
  try {
    if (process.platform !== "win32") process.kill(-child.pid, 0);
    else if (child.exitCode !== null || child.signalCode !== null) return false;
    return true;
  } catch {
    return false;
  }
}

export async function terminate(child: ChildProcess): Promise<void> {
  groupSignal(child, "SIGTERM");
  const deadline = Date.now() + 3000;
  while (groupAlive(child) && Date.now() < deadline) await delay(50);
  if (groupAlive(child)) groupSignal(child, "SIGKILL");
  if (child.exitCode === null && child.signalCode === null) {
    await Promise.race([
      new Promise<void>((done) => child.once("close", () => done())),
      delay(1000),
    ]);
  }
}

async function availablePort(port: number): Promise<void> {
  await new Promise<void>((resolvePort, reject) => {
    const server = net.createServer();
    server.once("error", () => reject(new Error(`本地端口 ${port} 已被占用`)));
    server.listen(port, "127.0.0.1", () => server.close(() => resolvePort()));
  });
}

export function validateLocal(config: LocalConfig): number {
  if (
    !config ||
    !["standalone", "replication", "cluster", "failover"].includes(config.mode)
  )
    throw new Error("实例模式无效");
  const count =
    config.mode === "standalone" ? 1 : config.mode === "replication" ? 2 : 3;
  integer(config.port, "起始 RESP 端口", 1, 65536 - count);
  integer(config.statsPort, "起始监控端口", 1, 65536 - count);
  const ports = Array.from({ length: count }, (_, index) => [
    config.port + index,
    config.statsPort + index,
  ]).flat();
  if (new Set(ports).size !== ports.length)
    throw new Error("RESP 端口和监控端口不能重叠");
  integer(config.ioThreads, "IO 线程数", 1, 128);
  integer(config.shards, "缓存分片数", 1, 1024);
  integer(config.memoryMiB, "内存额度", 0, 1_048_576);
  if (!["noeviction", "lru"].includes(config.eviction))
    throw new Error("淘汰策略无效");
  if (typeof config.password !== "string" || config.password.length > 4096)
    throw new Error("密码无效");
  return count;
}

interface OwnedNode {
  node: LocalNode;
  child: ChildProcess;
  password: string;
}

export class LocalManager {
  private readonly owned = new Map<string, OwnedNode>();
  private starting = false;
  private stopping = false;
  private controller: AbortController | null = null;

  constructor(
    private readonly directory: string,
    private readonly emit: (event: DesktopEvent) => void,
    private readonly logs: LogBook,
  ) {}

  list(): LocalNode[] {
    return [...this.owned.values()].map(({ node }) => ({ ...node }));
  }
  private publish(): void {
    this.emit({ type: "local", nodes: this.list() });
  }

  async start(config: LocalConfig): Promise<LocalNode[]> {
    if (
      this.starting ||
      this.stopping ||
      this.list().some((node) =>
        ["starting", "running", "stopping"].includes(node.state),
      )
    )
      throw new Error("请先停止正在运行的本地实例");
    const count = validateLocal(config);
    this.starting = true;
    const controller = new AbortController();
    this.controller = controller;
    this.owned.clear();
    const group = Array.from(
      { length: count },
      (_, index) => `127.0.0.1:${config.port + index}`,
    ).join(",");
    const master = `127.0.0.1:${config.port}`;
    try {
      if (process.platform !== "linux")
        throw new Error(
          "MiniRedis 原生服务端需要 Linux；当前系统可连接远程 Linux 实例",
        );
      const binary = await validateBinary(config.binary);
      controller.signal.throwIfAborted();
      await Promise.all(
        Array.from({ length: count }, (_, index) => [
          availablePort(config.port + index),
          availablePort(config.statsPort + index),
        ]).flat(),
      );
      controller.signal.throwIfAborted();
      for (let index = 0; index < count; index++) {
        controller.signal.throwIfAborted();
        const port = config.port + index;
        const statsPort = config.statsPort + index;
        const id = randomUUID();
        const label =
          config.mode === "cluster"
            ? `节点 ${index + 1}`
            : index === 0
              ? "Primary"
              : `Replica ${index}`;
        const data = join(this.directory, "instances", String(port));
        await mkdir(data, { recursive: true });
        controller.signal.throwIfAborted();
        const args = [
          "--bind",
          "127.0.0.1",
          "--port",
          String(port),
          "--stats-bind",
          "127.0.0.1",
          "--stats-port",
          String(statsPort),
          "--snapshot-file",
          join(data, "snapshot.dat"),
          "--snapshot-interval",
          "60",
          "--io-threads",
          String(config.ioThreads),
          "--cache-shards",
          String(config.shards),
          "--maxmemory",
          String(config.memoryMiB * 1024 * 1024),
          "--eviction-policy",
          config.eviction,
        ];
        if (config.aof)
          args.push(
            "--appendonly-file",
            join(data, "appendonly.aof"),
            "--appendfsync",
            "everysec",
          );
        if (config.mode === "cluster")
          args.push(
            "--cluster",
            "--node-addr",
            `127.0.0.1:${port}`,
            "--nodes",
            group,
            "--cluster-config-file",
            join(data, "cluster.conf"),
          );
        if (config.mode === "replication" || config.mode === "failover") {
          if (index) args.push("--replicaof", master);
          else
            args.push(
              "--replicas",
              Array.from(
                { length: count - 1 },
                (_, i) => `127.0.0.1:${config.port + i + 1}`,
              ).join(","),
            );
        }
        if (config.mode === "failover")
          args.push(
            "--automatic-failover",
            "--node-addr",
            `127.0.0.1:${port}`,
            "--failover-nodes",
            group,
            "--failover-state-file",
            join(data, "failover.state"),
          );
        const env = cleanEnvironment();
        if (config.password) env.MINIREDIS_REQUIREPASS = config.password;
        const child = spawn(binary, args, {
          env,
          detached: true,
          stdio: ["ignore", "pipe", "pipe"],
        });
        const node: LocalNode = {
          id,
          label,
          port,
          statsPort,
          state: "starting",
          pid: child.pid,
        };
        this.owned.set(id, { node, child, password: config.password });
        this.logs.pipe(child, `${label} · ${port}`, [config.password]);
        child.on("error", (error) => {
          node.state = "error";
          node.error = error.message;
          this.publish();
        });
        child.on("close", (code) => {
          node.state = node.state === "error" ? "error" : "stopped";
          this.logs.add(label, `进程退出，code=${code ?? "signal"}`);
          this.publish();
        });
        this.publish();
        const deadline = Date.now() + 8000;
        let ready = false;
        while (Date.now() < deadline) {
          controller.signal.throwIfAborted();
          if (node.state === "error" || node.state === "stopped")
            throw new Error(`${label} 启动失败，请查看日志`);
          const client = new RespConnection("127.0.0.1", port, 600);
          try {
            await client.open("", config.password);
            ready = true;
            break;
          } catch {
            await delay(100, undefined, { signal: controller.signal });
          } finally {
            client.close();
          }
        }
        if (!ready) throw new Error(`${label} 未在 8 秒内就绪`);
        controller.signal.throwIfAborted();
        node.state = "running";
        this.logs.add(label, `已就绪 · RESP ${port} / HTTP ${statsPort}`);
        this.publish();
      }
      return this.list();
    } catch (error) {
      await Promise.all(
        [...this.owned.values()].map(({ child }) => terminate(child)),
      );
      this.publish();
      throw new Error(
        controller.signal.aborted
          ? "本地实例启动已取消"
          : asError(error).message,
      );
    } finally {
      this.starting = false;
      if (this.controller === controller) this.controller = null;
    }
  }

  async stop(): Promise<void> {
    this.stopping = true;
    this.controller?.abort();
    try {
      await Promise.all(
        [...this.owned.values()].map(async ({ node, child }) => {
          if (!["running", "starting", "stopping"].includes(node.state)) return;
          node.state = "stopping";
          this.publish();
          await terminate(child);
          if (node.state === "stopping") node.state = "stopped";
        }),
      );
    } finally {
      this.stopping = false;
      this.publish();
    }
  }

  connection(id: string): { node: LocalNode; password: string } {
    const owned = this.owned.get(id);
    if (!owned || owned.node.state !== "running")
      throw new Error("本地节点尚未就绪");
    return { node: { ...owned.node }, password: owned.password };
  }

  async stopNode(id: string): Promise<void> {
    const owned = this.owned.get(id);
    if (!owned) throw new Error("本地节点不存在");
    owned.node.state = "stopping";
    this.publish();
    await terminate(owned.child);
    owned.node.state = "stopped";
    this.publish();
  }
}

async function taskPorts(): Promise<NodeJS.ProcessEnv> {
  for (let attempt = 0; attempt < 20; attempt++) {
    const base = 20000 + Math.floor(Math.random() * 20000);
    try {
      await Promise.all(
        Array.from({ length: 5 }, (_, index) => [
          availablePort(base + index),
          availablePort(base + 10 + index),
        ]).flat(),
      );
      return {
        PORT: String(base),
        STATS_PORT: String(base + 10),
        SMOKE_PORT: String(base),
        SMOKE_STATS_PORT: String(base + 10),
        SMOKE_CLUSTER_PORT: String(base + 1),
        SMOKE_CLUSTER_STATS_PORT: String(base + 11),
        MASTER_PORT: String(base),
        REPLICA_PORT: String(base + 1),
        REPLICA1_PORT: String(base + 1),
        REPLICA2_PORT: String(base + 2),
        MASTER_STATS_PORT: String(base + 10),
        REPLICA_STATS_PORT: String(base + 11),
        REPLICA1_STATS_PORT: String(base + 11),
        REPLICA2_STATS_PORT: String(base + 12),
        PORTS: `${base} ${base + 1} ${base + 2}`,
        BASE_STATS_PORT: String(base + 10),
      };
    } catch {
      /* Retry another range without changing a running local instance. */
    }
  }
  throw new Error("无法分配任务端口，请停止其他任务后重试");
}

const TASK_SCRIPTS: Record<TaskKind, string> = {
  benchmark: "scripts/benchmark.sh",
  smoke: "tests/integration/smoke.sh",
  recovery: "scripts/recovery_soak.sh",
  resources: "scripts/resource_failure_soak.sh",
  replication: "scripts/replica_demo.sh",
  cluster: "scripts/cluster_demo.sh",
  failover: "scripts/failover_demo.sh",
};

export class TaskManager {
  private child: ChildProcess | null = null;
  private state: TaskState | null = null;
  private starting = false;
  private cancelled = false;
  private generation = 0;
  constructor(
    private readonly projectRoot: string | null,
    private readonly directory: string,
    private readonly emit: (event: DesktopEvent) => void,
    private readonly logs: LogBook,
  ) {}
  current(): TaskState | null {
    return this.state ? { ...this.state } : null;
  }

  async run(input: TaskInput): Promise<TaskState> {
    if (this.child || this.starting) throw new Error("已有任务运行中");
    if (!this.projectRoot)
      throw new Error("脚本任务需要从 MiniRedis 源码目录运行桌面端");
    if (process.platform !== "linux")
      throw new Error("项目验证脚本需要 Linux 环境");
    if (!Object.hasOwn(TASK_SCRIPTS, input.kind))
      throw new Error("任务类型无效");
    integer(input.requests, "请求数", 1, 1_000_000);
    integer(input.clients, "客户端数", 1, 1000);
    integer(input.payload, "数据大小", 1, 1024 * 1024);
    integer(input.runs, "轮数", 1, 5);
    this.starting = true;
    this.cancelled = false;
    const generation = ++this.generation;
    try {
      const binary = await validateBinary(input.binary);
      const script = resolve(this.projectRoot, TASK_SCRIPTS[input.kind]);
      if (!isAbsolute(script)) throw new Error("脚本路径无效");
      await access(script);
      const id = randomUUID();
      const workdir = join(this.directory, "tasks", id);
      await mkdir(workdir, { recursive: true });
      const ports = await taskPorts();
      if (generation !== this.generation) throw new Error("任务启动已取消");
      const env: NodeJS.ProcessEnv = {
        ...cleanEnvironment(),
        ...ports,
        SERVER: binary,
        HOST: "127.0.0.1",
        WORKDIR: workdir,
        REQUESTS: String(input.requests),
        CLIENTS: String(input.clients),
        VALUE_SIZES: String(input.payload),
        RUNS: String(input.runs),
        WARMUP_REQUESTS: String(Math.min(1000, input.requests)),
        SOAK_SECONDS: "1",
        SAVE_RAW: "0",
        OUT: join(workdir, "benchmark-report.txt"),
        SUMMARY_OUT: join(workdir, "benchmark-summary.md"),
        DATA_OUT: join(workdir, "benchmark-runs.tsv"),
      };
      const args = [script];
      if (input.kind === "smoke") args.push(binary);
      if (["replication", "cluster", "failover"].includes(input.kind))
        args.push("smoke");
      this.state = {
        id,
        kind: input.kind,
        status: "running",
        startedAt: Date.now(),
      };
      const child = spawn("bash", args, {
        cwd: this.projectRoot,
        env,
        detached: true,
        stdio: ["ignore", "pipe", "pipe"],
      });
      this.child = child;
      this.logs.add(input.kind, `任务开始 · 结果目录 ${workdir}`);
      this.logs.pipe(child, input.kind);
      const finish = (exitCode: number | null, error?: Error): void => {
        if (this.child !== child || !this.state) return;
        this.child = null;
        this.state = {
          ...this.state,
          exitCode,
          status: this.cancelled
            ? "cancelled"
            : error || exitCode !== 0
              ? "failed"
              : "completed",
        };
        if (error) this.logs.add(input.kind, error.message);
        this.emit({ type: "task", task: this.state });
      };
      child.once("error", (error) => finish(null, error));
      child.once("close", (code) => finish(code));
      this.emit({ type: "task", task: this.state });
      return { ...this.state };
    } finally {
      this.starting = false;
    }
  }

  async stop(): Promise<void> {
    this.cancelled = true;
    this.generation++;
    if (this.child) await terminate(this.child);
  }
}
