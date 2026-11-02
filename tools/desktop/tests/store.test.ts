import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type {
  DesktopApi,
  DesktopEvent,
  MonitoringSnapshot,
} from "../src/shared/contracts";

const profile = (id: string) => ({
  id,
  name: id,
  host: "127.0.0.1",
  port: 6366,
  statsPort: 8080,
  username: "",
  color: "#123456",
  followRedirects: true,
  hasPassword: false,
});
const snapshot = (id = "a", total = 10): MonitoringSnapshot => ({
  connectionId: id,
  timestamp: Date.now(),
  healthy: true,
  ready: true,
  stats: {
    total_commands: total,
    key_count: 2,
    used_memory_bytes: 100,
    avg_command_latency_us: 12,
    connected_clients: 1,
  },
});
let listener: (event: DesktopEvent) => void;
let api: DesktopApi;
let mediaListener: (event: { matches: boolean }) => void;
async function flush() {
  for (let i = 0; i < 12; i++) await Promise.resolve();
}
async function setup(
  bootstrapConnected = false,
  bootstrapRunning = false,
  bootError = false,
) {
  vi.resetModules();
  vi.stubGlobal("matchMedia", () => ({
    matches: false,
    addEventListener: (_name: string, callback: typeof mediaListener) => {
      mediaListener = callback;
    },
  }));
  vi.stubGlobal("document", {
    createElement: () => ({}),
    documentElement: { dataset: {} },
  });
  const workspace = {
    profiles: [profile("a"), profile("b")],
    running: bootstrapRunning ? ["a"] : [],
    connections: bootstrapConnected
      ? [{ id: "a", status: "connected", connectedAt: 1 }]
      : [],
    theme: "light",
    selectedId: "a",
    binary: "",
    projectRoot: null,
    platform: "linux",
    passwordStorage: "session",
    version: "1.0.0",
  };
  api = {
    workspace: vi.fn().mockResolvedValue(workspace),
    localState: vi.fn().mockResolvedValue([]),
    taskState: vi.fn().mockResolvedValue(null),
    logs: vi.fn().mockResolvedValue([]),
    preferences: vi.fn().mockResolvedValue(undefined),
    onEvent: vi.fn((callback) => {
      listener = callback;
      return () => {};
    }),
    monitoring: vi.fn(async (id) => snapshot(id)),
    connect: vi.fn().mockResolvedValue(undefined),
    execute: vi.fn(async ({ connectionId, runId }) => {
      listener({
        type: "result",
        result: {
          id: crypto.randomUUID(),
          runId,
          connectionId,
          command: "PING",
          response: { type: "simple", value: "PONG" },
          durationMs: 1,
          timestamp: Date.now(),
          node: "localhost",
          redirects: [],
        },
      });
      return { count: 1, cancelled: false };
    }),
  } as unknown as DesktopApi;
  vi.stubGlobal("window", { desktop: api });
  if (bootError)
    vi.mocked(api.workspace).mockRejectedValue(
      new Error("workspace unavailable"),
    );
  const store = await import("../src/renderer/store");
  store.state.autoRefresh = false;
  await store.initialize();
  if (!bootstrapConnected)
    listener({ type: "connection", state: { id: "a", status: "connected" } });
  await flush();
  return store;
}

beforeEach(() => {
  vi.useFakeTimers();
  vi.setSystemTime(new Date("2026-11-02T00:00:00Z"));
});
afterEach(() => {
  vi.clearAllTimers();
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

describe("desktop view state", () => {
  it("restores and updates in-flight execution independently from an old renderer IPC request", async () => {
    const store = await setup(true, true);
    expect(store.running.value).toBe(true);
    listener({ type: "execution", connectionId: "a", running: false });
    expect(store.running.value).toBe(false);
  });
  it("does not let an older IPC completion clear a newer execution", async () => {
    const store = await setup();
    const finish: ((value: { count: number; cancelled: boolean }) => void)[] =
      [];
    vi.mocked(api.execute).mockImplementation(
      () =>
        new Promise((done) => {
          finish.push(done);
        }),
    );
    const first = store.execute("PING");
    listener({ type: "execution", connectionId: "a", running: false });
    const second = store.execute("PING");
    finish[0]({ count: 0, cancelled: false });
    await first;
    expect(store.running.value).toBe(true);
    finish[1]({ count: 0, cancelled: false });
    await second;
    expect(store.running.value).toBe(false);
  });
  it("removes deleted-profile caches even if a previous command completes later", async () => {
    const store = await setup();
    let finish!: (value: { count: number; cancelled: boolean }) => void;
    store.state.history.a = ["PING"];
    store.state.scripts.a = "PING";
    vi.mocked(api.execute).mockImplementationOnce(
      () =>
        new Promise((done) => {
          finish = done;
        }),
    );
    const work = store.execute("PING");
    store.forgetConnection("a");
    finish({ count: 0, cancelled: true });
    await work;
    expect(store.active.value?.id).toBe("b");
    expect(store.state.running.a).toBeUndefined();
    expect(store.state.history.a).toBeUndefined();
    expect(store.state.scripts.a).toBeUndefined();
  });
  it("reports bootstrap failures and a missing desktop bridge", async () => {
    const store = await setup(false, false, true);
    expect(store.state.bootError).toBe("workspace unavailable");
    expect(store.state.ready).toBe(false);
    vi.stubGlobal("window", {});
    await store.initialize();
    expect(store.state.bootError).toContain("npm run dev");
  });
  it("keeps newer connection attempts when an earlier request fails late", async () => {
    const store = await setup();
    let fail!: (error: Error) => void;
    vi.mocked(api.connect).mockImplementationOnce(
      () =>
        new Promise((_done, reject) => {
          fail = reject;
        }),
    );
    const old = store.connectProfile("a");
    await store.connectProfile("a");
    expect(api.connect).toHaveBeenCalledTimes(1);
    listener({
      type: "connection",
      state: { id: "a", status: "disconnected" },
    });
    vi.mocked(api.connect).mockImplementationOnce(async () => {
      listener({ type: "connection", state: { id: "a", status: "connected" } });
    });
    await store.connectProfile("a");
    fail(new Error("old failure"));
    await old;
    expect(store.connected.value).toBe(true);
    expect(store.state.notices.at(-1)?.message).toContain("已连接到");
  });
  it("restores the connection already owned by main after renderer reload", async () => {
    const store = await setup(true);
    expect(store.connected.value).toBe(true);
    expect(store.stats.value?.key_count).toBe(2);
  });
  it("shows no current measurements after disconnect or a failed sample", async () => {
    const store = await setup();
    expect(store.stats.value?.key_count).toBe(2);
    listener({
      type: "connection",
      state: { id: "a", status: "disconnected" },
    });
    await flush();
    expect(store.stats.value).toBeUndefined();
    expect(store.samples.value).toEqual([]);
    listener({ type: "connection", state: { id: "a", status: "connected" } });
    await flush();
    vi.mocked(api.monitoring).mockResolvedValue({
      connectionId: "a",
      timestamp: Date.now(),
      error: "HTTP failed",
    });
    store.refreshMonitoring();
    await flush();
    expect(store.samples.value).toEqual([]);
  });
  it("calculates QPS from actual deltas and treats reset counters as unknown", async () => {
    const store = await setup();
    expect(store.samples.value[0].qps).toBeNull();
    await vi.advanceTimersByTimeAsync(2500);
    vi.mocked(api.monitoring).mockResolvedValue(snapshot("a", 20));
    store.refreshMonitoring();
    await flush();
    expect(store.samples.value.at(-1)?.qps).toBe(4);
    await vi.advanceTimersByTimeAsync(2500);
    vi.mocked(api.monitoring).mockResolvedValue(snapshot("a", 1));
    store.refreshMonitoring();
    await flush();
    expect(store.samples.value.at(-1)?.qps).toBeNull();
  });
  it("does not invent zero measurements when fields are absent", async () => {
    const store = await setup();
    vi.mocked(api.monitoring).mockResolvedValue({
      connectionId: "a",
      timestamp: Date.now(),
      stats: { key_count: 1 },
    });
    store.refreshMonitoring();
    await flush();
    expect(store.samples.value.at(-1)).toMatchObject({
      total: null,
      memory: null,
      latency: null,
      clients: null,
      qps: null,
    });
  });
  it("discards late samples from a different selected profile", async () => {
    const store = await setup();
    let resolveOld!: (value: MonitoringSnapshot) => void;
    vi.mocked(api.monitoring).mockImplementationOnce(
      () =>
        new Promise((done) => {
          resolveOld = done;
        }),
    );
    store.refreshMonitoring();
    store.state.connections.b = { id: "b", status: "connected" };
    store.state.selectedId = "b";
    await flush();
    resolveOld({ ...snapshot("a"), stats: { key_count: 999 } });
    await flush();
    expect(store.stats.value?.key_count).toBe(2);
    expect(store.snapshot.value?.connectionId).toBe("b");
  });
  it("stops periodic polling when auto refresh is disabled", async () => {
    const store = await setup();
    store.state.autoRefresh = true;
    await flush();
    await vi.advanceTimersByTimeAsync(2500);
    expect(vi.mocked(api.monitoring).mock.calls.length).toBeGreaterThan(1);
    store.state.autoRefresh = false;
    await flush();
    const count = vi.mocked(api.monitoring).mock.calls.length;
    await vi.advanceTimersByTimeAsync(10000);
    expect(vi.mocked(api.monitoring)).toHaveBeenCalledTimes(count);
  });
  it("retains all duplicate responses up to the documented limit, without AUTH history", async () => {
    const store = await setup();
    let index = 0;
    vi.mocked(api.execute).mockImplementation(
      async ({ connectionId, runId }) => {
        for (let i = 0; i < 200; i++)
          listener({
            type: "result",
            result: {
              id: String(index++),
              runId,
              connectionId,
              command: i === 0 ? "AUTH ••••••" : "PING",
              response: { type: "simple", value: "OK" },
              timestamp: Date.now(),
              durationMs: 1,
              node: "localhost",
              redirects: [],
            },
          });
        return { count: 200, cancelled: false };
      },
    );
    await store.execute("AUTH private-fixture\n" + "PING\n".repeat(199));
    expect(store.results.value).toHaveLength(100);
    expect(store.state.history.a).toBeUndefined();
    await store.execute("PING");
    expect(store.state.history.a).toEqual(["PING"]);
  });
  it("isolates pending execution from profile selection and suppresses duplicate runs", async () => {
    const store = await setup();
    let finish!: () => void;
    vi.mocked(api.execute).mockImplementationOnce(
      () =>
        new Promise((done) => {
          finish = () => done({ count: 0, cancelled: true });
        }),
    );
    const running = store.execute("PING");
    await store.execute("PING");
    expect(api.execute).toHaveBeenCalledTimes(1);
    store.state.selectedId = "b";
    finish();
    await running;
    expect(store.state.running.a).toBe(false);
    expect(store.state.history.b).toBeUndefined();
  });
  it("rejects offline execution and recovers after an IPC error", async () => {
    const store = await setup();
    store.state.connections.a.status = "disconnected";
    await store.execute("PING");
    expect(api.execute).not.toHaveBeenCalled();
    store.state.connections.a.status = "connected";
    vi.mocked(api.execute).mockRejectedValueOnce(
      new Error(
        "Error invoking remote method 'desktop:execute': Error: failure",
      ),
    );
    await store.execute("PING");
    expect(store.state.running.a).toBe(false);
    expect(store.state.notices.at(-1)?.message).toBe("failure");
    await store.execute("PING");
    expect(store.results.value).toHaveLength(1);
  });
  it("does not replace an explicitly disconnected connection with a stale connect error", async () => {
    const store = await setup();
    let reject!: (reason: Error) => void;
    vi.mocked(api.connect).mockImplementationOnce(
      () =>
        new Promise((_done, fail) => {
          reject = fail;
        }),
    );
    const connecting = store.connectProfile("a");
    listener({
      type: "connection",
      state: { id: "a", status: "disconnected" },
    });
    reject(new Error("连接已取消"));
    await connecting;
    expect(store.state.connections.a.status).toBe("disconnected");
  });
  it("persists all theme choices and responds to system color changes", async () => {
    const store = await setup();
    await store.setTheme("dark");
    await flush();
    expect(store.isDark.value).toBe(true);
    await store.setTheme("system");
    mediaListener({ matches: true });
    await flush();
    expect(store.isDark.value).toBe(true);
    mediaListener({ matches: false });
    await flush();
    expect(store.isDark.value).toBe(false);
    vi.mocked(api.preferences).mockRejectedValueOnce(new Error("disk error"));
    await store.setTheme("light");
    expect(store.state.notices.at(-1)?.message).toBe("disk error");
  });
  it("handles log/task events, bounded notifications and display formatting", async () => {
    const store = await setup();
    for (let i = 0; i < 505; i++)
      listener({
        type: "log",
        entry: {
          id: String(i),
          source: "qa",
          text: "line",
          timestamp: Date.now(),
        },
      });
    expect(store.state.logs).toHaveLength(500);
    listener({
      type: "task",
      task: { id: "t", kind: "smoke", status: "failed", startedAt: Date.now() },
    });
    expect(store.state.notices.at(-1)?.type).toBe("error");
    listener({
      type: "task",
      task: {
        id: "t",
        kind: "smoke",
        status: "completed",
        startedAt: Date.now(),
      },
    });
    expect(store.state.notices.at(-1)?.type).toBe("success");
    for (let i = 0; i < 10; i++) store.notice(String(i));
    expect(store.state.notices).toHaveLength(4);
    await vi.advanceTimersByTimeAsync(7001);
    expect(store.state.notices).toEqual([]);
    expect(store.bytes(undefined)).toBe("—");
    expect(store.bytes(NaN)).toBe("—");
    expect(store.bytes(1024)).toBe("1 KiB");
    expect(store.number(undefined)).toBe("—");
    expect(store.number(12)).toBe("12");
  });
});
