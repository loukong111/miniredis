import net from "node:net";
import { spawn } from "node:child_process";
import { PassThrough } from "node:stream";
import { mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { afterEach, describe, expect, it } from "vitest";
import {
  LocalManager,
  LogBook,
  TaskManager,
  validateLocal,
  terminate,
} from "../src/main/processes";
import { monitoring, RedisSession } from "../src/main/redis";
import type {
  CommandResult,
  ConnectionProfile,
  DesktopEvent,
  LocalConfig,
} from "../src/shared/contracts";

const cleanups: (() => Promise<void>)[] = [];
afterEach(async () => {
  for (const close of cleanups.splice(0).reverse()) await close();
});
async function directory(): Promise<string> {
  const path = await mkdtemp(join(tmpdir(), "miniredis-processes-"));
  cleanups.push(() => rm(path, { force: true, recursive: true }));
  return path;
}
async function freePort(): Promise<number> {
  const server = net.createServer();
  await new Promise<void>((done) => server.listen(0, "127.0.0.1", done));
  const port = (server.address() as net.AddressInfo).port;
  await new Promise<void>((done) => server.close(() => done()));
  return port;
}
function config(port: number, statsPort: number): LocalConfig {
  return {
    binary: resolve(
      process.env.MINIREDIS_TEST_BINARY || "../../build-review/miniredis",
    ),
    mode: "standalone",
    port,
    statsPort,
    password: "local-test-fixture",
    aof: true,
    ioThreads: 2,
    shards: 4,
    memoryMiB: 16,
    eviction: "noeviction",
  };
}
async function until(
  predicate: () => boolean | Promise<boolean>,
): Promise<void> {
  for (let i = 0; i < 100; i++) {
    if (await predicate()) return;
    await delay(50);
  }
  throw new Error("condition timed out");
}

describe("owned processes", () => {
  it.each([
    { mode: "bad" },
    { port: 0 },
    { statsPort: 65536 },
    { ioThreads: 0 },
    { shards: 0 },
    { memoryMiB: -1 },
    { eviction: "invalid" },
    { password: "x".repeat(4097) },
  ])("rejects invalid local configuration", (changes) => {
    expect(() =>
      validateLocal({ ...config(24000, 24100), ...changes } as LocalConfig),
    ).toThrow();
  });
  it("does not touch an unrelated process listening on a requested port", async () => {
    const listener = net.createServer();
    await new Promise<void>((done) => listener.listen(0, "127.0.0.1", done));
    cleanups.push(
      () => new Promise<void>((done) => listener.close(() => done())),
    );
    const local = new LocalManager(
      await directory(),
      () => {},
      new LogBook(() => {}),
    );
    await expect(
      local.start(
        config((listener.address() as net.AddressInfo).port, await freePort()),
      ),
    ).rejects.toThrow("被占用");
    expect(listener.listening).toBe(true);
    expect(local.list()).toEqual([]);
    await expect(local.stopNode("absent")).rejects.toThrow("不存在");
    expect(() => local.connection("absent")).toThrow("未就绪");
  });
  it("forces termination when an owned process ignores SIGTERM", async () => {
    const child = spawn(
      process.execPath,
      [
        "-e",
        "process.on('SIGTERM',()=>{});process.stdout.write('ready');setInterval(()=>{},1000)",
      ],
      { detached: true, stdio: ["ignore", "pipe", "pipe"] },
    );
    cleanups.push(() => terminate(child));
    await new Promise((done) => child.stdout.once("data", done));
    await terminate(child);
    expect(child.signalCode).toBe("SIGKILL");
  });
  it("redacts split UTF-8 log streams and bounds retained log entries", async () => {
    const logs = new LogBook(() => {});
    const stdout = new PassThrough();
    const stderr = new PassThrough();
    logs.pipe({ stdout, stderr } as never, "fixture", ["private-fixture"]);
    const bytes = Buffer.from("你好 private-fixture\n");
    stdout.write(bytes.subarray(0, 2));
    stdout.write(bytes.subarray(2, 12));
    stdout.end(bytes.subarray(12));
    stderr.end("last line");
    await delay(0);
    expect(
      logs
        .list()
        .map((entry) => entry.text)
        .join("\n"),
    ).toContain("你好 ••••••");
    expect(JSON.stringify(logs.list())).not.toContain("private-fixture");
    for (let i = 0; i < 510; i++) logs.add("fixture", "x".repeat(20000));
    expect(logs.list()).toHaveLength(500);
    expect(logs.list()[0].text).toHaveLength(16384);
  });
  it("rejects missing source roots, unsupported tasks and invalid parameters", async () => {
    const input = {
      kind: "benchmark" as const,
      binary: "/bin/true",
      requests: 1,
      clients: 1,
      payload: 1,
      runs: 1,
    };
    const path = await directory();
    const absent = new TaskManager(null, path, () => {}, new LogBook(() => {}));
    await expect(absent.run(input)).rejects.toThrow("源码目录");
    const tasks = new TaskManager(path, path, () => {}, new LogBook(() => {}));
    for (const changes of [
      { kind: "unknown" },
      { requests: 0 },
      { clients: 0 },
      { payload: 0 },
      { runs: 6 },
    ])
      await expect(
        tasks.run({ ...input, ...changes } as typeof input),
      ).rejects.toThrow();
    await expect(tasks.run({ ...input, binary: "/missing" })).rejects.toThrow(
      "不存在",
    );
  });
  it("rejects overlapping port ranges", () => {
    expect(() =>
      validateLocal({ ...config(30000, 30001), mode: "cluster" }),
    ).toThrow("不能重叠");
  });
  it("starts a real authenticated server, reads metrics and stops it", async () => {
    const events: DesktopEvent[] = [];
    const emit = (event: DesktopEvent): void => {
      events.push(event);
    };
    const local = new LocalManager(await directory(), emit, new LogBook(emit));
    cleanups.push(() => local.stop());
    const settings = config(await freePort(), await freePort());
    const [node] = await local.start(settings);
    expect(node.state).toBe("running");
    expect(local.connection(node.id).password).toBe(settings.password);
    const profile: ConnectionProfile = {
      id: node.id,
      name: "Actual MiniRedis",
      host: "127.0.0.1",
      port: node.port,
      statsPort: node.statsPort,
      username: "",
      hasPassword: true,
      followRedirects: true,
      color: "#123456",
    };
    const connection = new RedisSession(profile, "", settings.password, emit);
    cleanups.push(async () => connection.disconnect());
    await connection.connect();
    await connection.execute(
      'SET live:test "你好"\nGET live:test\nGET live:test\nAUTH local-test-fixture',
      "live",
    );
    const results = events.filter(
      (event): event is { type: "result"; result: CommandResult } =>
        event.type === "result",
    );
    expect(results).toHaveLength(4);
    expect(results[1].result.response).toEqual({ type: "bulk", value: "你好" });
    expect(results[2].result.response).toEqual(results[1].result.response);
    expect(JSON.stringify(events)).not.toContain(settings.password);
    const snapshot = await monitoring(profile);
    expect(snapshot.error).toBeUndefined();
    expect(snapshot.healthy).toBe(true);
    expect(snapshot.ready).toBe(true);
    await until(
      async () => Number((await monitoring(profile)).stats?.key_count) > 0,
    );
    expect((await connection.topology()).replication.role).toBe("master");
    await local.stop();
    expect(local.list()[0].state).toBe("stopped");
    await expect(
      new Promise<void>((done, reject) => {
        const socket = net.connect(node.port, "127.0.0.1", () => {
          socket.destroy();
          done();
        });
        socket.on("error", reject);
      }),
    ).rejects.toThrow();
  });
  it("cancels a local start while binary validation is pending", async () => {
    const emit = (): void => {};
    const local = new LocalManager(await directory(), emit, new LogBook(emit));
    cleanups.push(() => local.stop());
    const start = local.start(config(await freePort(), await freePort()));
    const cancelled = expect(start).rejects.toThrow("取消");
    await local.stop();
    await cancelled;
    expect(local.list()).toEqual([]);
  });
  it.each(["cluster", "failover"] as const)(
    "starts and stops all three nodes in %s mode",
    async (mode) => {
      const directoryPath = await directory();
      const local = new LocalManager(
        directoryPath,
        () => {},
        new LogBook(() => {}),
      );
      cleanups.push(() => local.stop());
      // Random ranges avoid the integration suite's default ports.
      const base = 22000 + Math.floor(Math.random() * 10000);
      const nodes = await local.start({ ...config(base, base + 10), mode });
      expect(nodes).toHaveLength(3);
      expect(nodes.every((node) => node.state === "running")).toBe(true);
      const profile: ConnectionProfile = {
        id: "multi",
        name: mode,
        host: "127.0.0.1",
        port: base,
        statsPort: base + 10,
        username: "",
        hasPassword: true,
        followRedirects: true,
        color: "#123456",
      };
      const connection = new RedisSession(
        profile,
        "",
        "local-test-fixture",
        () => {},
      );
      cleanups.push(async () => connection.disconnect());
      await connection.connect();
      if (mode === "cluster")
        expect((await connection.topology()).nodes).toHaveLength(3);
      else
        expect((await monitoring(profile)).stats?.failover_enabled).toBe(true);
      await local.stop();
      expect(local.list().every((node) => node.state === "stopped")).toBe(true);
    },
  );
  it("cancels task preparation and terminates the script process group", async () => {
    const path = await directory();
    await mkdir(join(path, "scripts"));
    await writeFile(
      join(path, "scripts/benchmark.sh"),
      'set -eu\nsleep 60 &\necho $! > "$WORKDIR/child.pid"\nwait\n',
    );
    const task = new TaskManager(path, path, () => {}, new LogBook(() => {}));
    cleanups.push(() => task.stop());
    const input = {
      kind: "benchmark" as const,
      binary: "/bin/true",
      requests: 10,
      clients: 1,
      payload: 8,
      runs: 1,
    };
    const preparing = task.run(input);
    const cancelled = expect(preparing).rejects.toThrow("取消");
    await task.stop();
    await cancelled;
    const state = await task.run(input);
    const pidFile = join(path, "tasks", state.id, "child.pid");
    await until(async () =>
      Boolean(await readFile(pidFile, "utf8").catch(() => "")),
    );
    const pid = Number(await readFile(pidFile, "utf8"));
    await task.stop();
    await until(() => task.current()?.status === "cancelled");
    // A terminated orphan can briefly remain a zombie until the host reaps it.
    const processState = await readFile(`/proc/${pid}/stat`, "utf8").catch(
      () => "",
    );
    expect(!processState || processState.split(" ")[2] === "Z").toBe(true);
  });
});
