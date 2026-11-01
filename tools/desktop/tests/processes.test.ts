import net from "node:net";
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
