import { randomUUID } from "node:crypto";
import net from "node:net";
import type {
  CommandResult,
  ConnectionProfile,
  ConnectionState,
  DesktopEvent,
  MonitoringSnapshot,
  RespValue,
  Stats,
  TopologySnapshot,
} from "../shared/contracts";
import { encodeCommand, RespDecoder } from "./protocol";
import { displayCommand, parseScript } from "../shared/commands";

interface Pending {
  resolve: (value: RespValue) => void;
  reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

export class RespConnection {
  private readonly socket = new net.Socket();
  private readonly decoder = new RespDecoder();
  private readonly pending: Pending[] = [];
  private closed = false;
  onDisconnect?: (reason: Error) => void;

  constructor(
    readonly host: string,
    readonly port: number,
    private readonly timeout = 5000,
  ) {
    this.socket.setNoDelay(true);
    this.socket.on("data", (data) => {
      try {
        for (const value of this.decoder.feed(data)) {
          const request = this.pending.shift();
          if (!request) throw new Error("收到未请求的 RESP 响应");
          clearTimeout(request.timer);
          request.resolve(value);
        }
      } catch (error) {
        this.close(asError(error));
      }
    });
    this.socket.on("error", (error) => this.close(error));
    this.socket.on("close", () => this.close(new Error("连接已关闭")));
  }

  async open(username = "", password = ""): Promise<void> {
    await new Promise<void>((resolve, reject) => {
      const cleanup = (): void => {
        clearTimeout(timer);
        this.socket.off("connect", connected);
        this.socket.off("error", failed);
        this.socket.off("close", disconnected);
      };
      const connected = (): void => {
        cleanup();
        resolve();
      };
      const failed = (error: Error): void => {
        cleanup();
        reject(error);
      };
      const disconnected = (): void =>
        failed(new Error("建立连接时连接已关闭"));
      const timer = setTimeout(() => {
        failed(new Error("连接超时"));
        this.close();
      }, this.timeout);
      this.socket.once("connect", connected);
      this.socket.once("error", failed);
      this.socket.once("close", disconnected);
      this.socket.connect(this.port, this.host);
    });
    try {
      if (username || password) {
        const response = await this.request(
          username ? ["AUTH", username, password] : ["AUTH", password],
        );
        if (response.type === "error")
          throw new Error("认证失败，请检查用户名和密码");
      }
      const response = await this.request(["PING"]);
      if (response.type === "error") throw new Error(response.value);
    } catch (error) {
      this.close(asError(error));
      throw error;
    }
  }

  request(parts: string[]): Promise<RespValue> {
    if (this.closed || !this.socket.writable)
      return Promise.reject(new Error("连接已断开，请重新连接"));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => this.close(new Error("命令执行超时，连接已断开以保持响应顺序")),
        this.timeout,
      );
      this.pending.push({ resolve, reject, timer });
      this.socket.write(encodeCommand(parts), (error) => {
        if (error) this.close(error);
      });
    });
  }

  close(reason = new Error("连接已关闭")): void {
    if (this.closed) return;
    this.closed = true;
    this.socket.destroy();
    for (const pending of this.pending.splice(0)) {
      clearTimeout(pending.timer);
      pending.reject(reason);
    }
    this.onDisconnect?.(reason);
  }
}

export function asError(error: unknown): Error {
  return error instanceof Error ? error : new Error(String(error));
}

export function parseAddress(address: string): { host: string; port: number } {
  const match = /^(?:\[([^\]]+)\]|([^:\s]+)):(\d+)$/.exec(address);
  const port = Number(match?.[3]);
  if (!match || port < 1 || port > 65535)
    throw new Error("服务器返回了无效的重定向地址");
  return { host: match[1] || match[2], port };
}

export function parseInfo(text: string): Record<string, string> {
  const info: Record<string, string> = {};
  for (const line of text.split(/\r?\n/)) {
    if (!line || line.startsWith("#")) continue;
    const colon = line.indexOf(":");
    if (colon > 0) info[line.slice(0, colon)] = line.slice(colon + 1);
  }
  return info;
}

export class RedisSession {
  private primary: RespConnection | null = null;
  private readonly transports = new Map<string, RespConnection>();
  private tail: Promise<unknown> = Promise.resolve();
  private generation = 0;
  private controller: AbortController | null = null;
  private activeTransport: RespConnection | null = null;
  state: ConnectionState;

  get executing(): boolean {
    return this.controller !== null;
  }

  constructor(
    readonly profile: ConnectionProfile,
    private username: string,
    private password: string,
    private readonly emit: (event: DesktopEvent) => void,
  ) {
    this.state = { id: profile.id, status: "disconnected" };
  }

  private setState(status: ConnectionState["status"], error?: string): void {
    this.state = {
      id: this.profile.id,
      status,
      error,
      connectedAt:
        status === "connected"
          ? (this.state.connectedAt ?? Date.now())
          : undefined,
      role: status === "connected" ? this.state.role : undefined,
    };
    this.emit({ type: "connection", state: this.state });
  }

  async connect(): Promise<void> {
    this.disconnect();
    const generation = ++this.generation;
    this.setState("connecting");
    const connection = new RespConnection(this.profile.host, this.profile.port);
    this.primary = connection;
    connection.onDisconnect = (error) => {
      if (this.primary === connection && this.state.status === "connected")
        this.setState("error", error.message);
    };
    try {
      await connection.open(this.username, this.password);
      if (generation !== this.generation) throw new Error("连接已取消");
      this.transports.set(
        `${this.profile.host}:${this.profile.port}`,
        connection,
      );
      const metadata = await connection.request(["INFO", "replication"]);
      if (generation !== this.generation) throw new Error("连接已取消");
      if (metadata.type === "bulk") this.updateRole(parseInfo(metadata.value));
      this.setState("connected");
    } catch (error) {
      connection.close();
      if (generation === this.generation)
        this.setState("error", asError(error).message);
      throw error;
    }
  }

  private updateRole(info: Record<string, string>): void {
    const role =
      info.role === "master"
        ? "primary"
        : info.role === "slave"
          ? "replica"
          : undefined;
    if (this.state.role === role) return;
    this.state.role = role;
    if (this.state.status === "connected")
      this.emit({ type: "connection", state: this.state });
  }

  disconnect(): void {
    this.generation++;
    this.controller?.abort();
    const primary = this.primary;
    this.primary = null;
    primary?.close();
    for (const connection of this.transports.values()) connection.close();
    this.transports.clear();
    this.setState("disconnected");
  }

  cancel(): void {
    this.controller?.abort();
    this.activeTransport?.close(new Error("执行已取消"));
  }

  private serialize<T>(operation: () => Promise<T>): Promise<T> {
    const result = this.tail.then(operation, operation);
    this.tail = result.catch(() => undefined);
    return result;
  }

  private async transport(host: string, port: number): Promise<RespConnection> {
    const address = `${host}:${port}`;
    let connection = this.transports.get(address);
    if (!connection) {
      const generation = this.generation;
      connection = new RespConnection(host, port);
      this.activeTransport = connection;
      await connection.open(this.username, this.password);
      if (generation !== this.generation || this.controller?.signal.aborted) {
        connection.close();
        throw new Error("连接已取消");
      }
      const owned = connection;
      connection.onDisconnect = () => {
        if (this.transports.get(address) === owned)
          this.transports.delete(address);
      };
      this.transports.set(address, connection);
    }
    return connection;
  }

  private async command(
    parts: string[],
    signal?: AbortSignal,
  ): Promise<{ response: RespValue; node: string; redirects: string[] }> {
    if (!this.primary || this.state.status !== "connected")
      throw new Error("请先连接一个实例");
    let connection = this.primary;
    let node = `${this.profile.host}:${this.profile.port}`;
    const redirects: string[] = [];
    let asking = false;
    for (let hop = 0; hop <= 3; hop++) {
      signal?.throwIfAborted();
      this.activeTransport = connection;
      if (asking) {
        const answer = await connection.request(["ASKING"]);
        if (answer.type === "error")
          return { response: answer, node, redirects };
      }
      const response = await connection.request(parts);
      signal?.throwIfAborted();
      const redirect =
        response.type === "error"
          ? /^(?:ERR )?(MOVED|ASK) \d+ (\S+)$/.exec(response.value)
          : null;
      if (!redirect || !this.profile.followRedirects)
        return { response, node, redirects };
      if (hop === 3) throw new Error("重定向超过 3 次，请检查集群拓扑");
      const target = parseAddress(redirect[2]);
      node = redirect[2];
      asking = redirect[1] === "ASK";
      redirects.push(`${redirect[1]} → ${node}`);
      connection = await this.transport(target.host, target.port);
    }
    throw new Error("无法完成命令");
  }

  async execute(
    script: string,
    runId: string,
  ): Promise<{ count: number; cancelled: boolean }> {
    const commands = parseScript(script);
    if (this.controller) throw new Error("该连接已有命令正在执行");
    const controller = new AbortController();
    this.controller = controller;
    this.emit({
      type: "execution",
      connectionId: this.profile.id,
      running: true,
    });
    const generation = this.generation;
    return this.serialize(async () => {
      let count = 0;
      try {
        for (const parts of commands) {
          if (controller.signal.aborted || generation !== this.generation)
            break;
          const started = performance.now();
          let outcome: Awaited<ReturnType<RedisSession["command"]>>;
          let transportError = false;
          try {
            outcome = await this.command(parts, controller.signal);
            if (
              parts[0]?.toUpperCase() === "AUTH" &&
              outcome.response.type !== "error"
            ) {
              this.username = parts.length > 2 ? parts[1] : "";
              this.password = parts.at(-1) ?? "";
              for (const [address, connection] of this.transports) {
                if (connection !== this.primary) {
                  connection.close();
                  this.transports.delete(address);
                }
              }
            }
          } catch (error) {
            if (controller.signal.aborted) break;
            transportError = true;
            outcome = {
              response: { type: "error", value: asError(error).message },
              node: `${this.profile.host}:${this.profile.port}`,
              redirects: [],
            };
          }
          const result: CommandResult = {
            id: randomUUID(),
            runId,
            connectionId: this.profile.id,
            command: displayCommand(parts),
            ...outcome,
            transportError,
            durationMs: performance.now() - started,
            timestamp: Date.now(),
          };
          this.emit({ type: "result", result });
          count++;
          if (transportError) break;
        }
        return {
          count,
          cancelled:
            controller.signal.aborted || generation !== this.generation,
        };
      } finally {
        this.activeTransport = null;
        if (this.controller === controller) {
          this.controller = null;
          this.emit({
            type: "execution",
            connectionId: this.profile.id,
            running: false,
          });
        }
      }
    });
  }

  topology(): Promise<TopologySnapshot> {
    return this.serialize(async () => {
      const replication = (await this.command(["INFO", "replication"]))
        .response;
      if (replication.type === "error") throw new Error(replication.value);
      if (replication.type === "bulk")
        this.updateRole(parseInfo(replication.value));
      const clusterInfo = (await this.command(["INFO", "cluster"])).response;
      const clusterEnabled =
        clusterInfo.type === "bulk"
          ? parseInfo(clusterInfo.value).cluster_enabled
          : undefined;
      const cluster = (await this.command(["CLUSTER", "NODES"])).response;
      return {
        clusterEnabled:
          clusterEnabled === undefined ? undefined : clusterEnabled === "1",
        replication:
          replication.type === "bulk" ? parseInfo(replication.value) : {},
        clusterError:
          cluster.type === "error"
            ? cluster.value.replace(/^ERR /, "")
            : undefined,
        nodes:
          cluster.type === "bulk"
            ? cluster.value
                .trim()
                .split("\n")
                .filter(Boolean)
                .map((line) => {
                  const columns = line.trim().split(/\s+/);
                  const flags = columns[2]?.split(",") ?? [];
                  return {
                    id: columns[0],
                    address: columns[1],
                    role: flags.includes("slave") ? "replica" : "primary",
                    state: flags.includes("fail")
                      ? "failed"
                      : flags.includes("pfail")
                        ? "suspect"
                        : "healthy",
                    slots: columns.slice(8).join(" "),
                    current: flags.includes("myself"),
                  };
                })
            : [],
      };
    });
  }
}

export async function fetchEndpoint(
  profile: ConnectionProfile,
  path: string,
): Promise<{ status: number; body: string }> {
  const host = profile.host.includes(":") ? `[${profile.host}]` : profile.host;
  const response = await fetch(`http://${host}:${profile.statsPort}${path}`, {
    signal: AbortSignal.timeout(4000),
    redirect: "error",
  });
  const reader = response.body?.getReader();
  const chunks: Uint8Array[] = [];
  let size = 0;
  try {
    if (reader)
      for (;;) {
        const chunk = await reader.read();
        if (chunk.done) break;
        size += chunk.value.length;
        if (size > 1024 * 1024) throw new Error("监控响应超过 1 MiB 限制");
        chunks.push(chunk.value);
      }
  } finally {
    await reader?.cancel().catch(() => undefined);
  }
  return {
    status: response.status,
    body: Buffer.concat(chunks).toString("utf8"),
  };
}

export async function monitoring(
  profile: ConnectionProfile,
): Promise<MonitoringSnapshot> {
  const [stats, health, ready] = await Promise.allSettled(
    ["/stats", "/healthz", "/readyz"].map((path) =>
      fetchEndpoint(profile, path),
    ),
  );
  const snapshot: MonitoringSnapshot = {
    connectionId: profile.id,
    timestamp: Date.now(),
  };
  if (health.status === "fulfilled")
    snapshot.healthy = health.value.status === 200;
  if (ready.status === "fulfilled") snapshot.ready = ready.value.status === 200;
  try {
    if (stats.status === "rejected") throw stats.reason;
    if (stats.value.status !== 200)
      throw new Error(`/stats 返回 HTTP ${stats.value.status}`);
    const value: unknown = JSON.parse(stats.value.body);
    if (!value || Array.isArray(value) || typeof value !== "object")
      throw new Error("监控数据格式无效");
    snapshot.stats = Object.fromEntries(
      Object.entries(value).filter(([, item]) =>
        ["string", "number", "boolean"].includes(typeof item),
      ),
    ) as Stats;
  } catch (error) {
    snapshot.error = `无法读取 ${profile.host}:${profile.statsPort} 的监控数据：${asError(error).message}`;
  }
  return snapshot;
}
