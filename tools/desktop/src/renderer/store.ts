import { computed, reactive, watch } from "vue";
import { parseScript } from "../shared/commands";
import type {
  CommandResult,
  ConnectionProfile,
  ConnectionState,
  LocalNode,
  LogEntry,
  MonitoringSnapshot,
  TaskState,
  Theme,
  Workspace,
} from "../shared/contracts";

export interface Sample {
  time: number;
  total: number | null;
  qps: number | null;
  memory: number | null;
  latency: number | null;
  clients: number | null;
}

export const state = reactive({
  workspace: null as Workspace | null,
  selectedId: null as string | null,
  page: "workbench" as "workbench" | "monitoring" | "topology",
  theme: "light" as Theme,
  systemDark: matchMedia("(prefers-color-scheme: dark)").matches,
  profiles: [] as ConnectionProfile[],
  connections: {} as Record<string, ConnectionState>,
  results: {} as Record<string, CommandResult[]>,
  history: {} as Record<string, string[]>,
  scripts: {} as Record<string, string>,
  snapshots: {} as Record<string, MonitoringSnapshot>,
  samples: {} as Record<string, Sample[]>,
  running: {} as Record<string, boolean>,
  nodes: [] as LocalNode[],
  task: null as TaskState | null,
  logs: [] as LogEntry[],
  tools: null as "local" | "tasks" | "settings" | null,
  logOpen: false,
  monitorLoading: false,
  autoRefresh: true,
  bootError: "",
  ready: false,
  notices: [] as {
    id: string;
    message: string;
    type: "success" | "error" | "info";
  }[],
});

export const active = computed(
  () =>
    state.profiles.find((profile) => profile.id === state.selectedId) ?? null,
);
export const connection = computed(() =>
  state.selectedId ? state.connections[state.selectedId] : undefined,
);
export const connected = computed(
  () => connection.value?.status === "connected",
);
export const snapshot = computed(() =>
  connected.value && state.selectedId
    ? state.snapshots[state.selectedId]
    : undefined,
);
export const stats = computed(() => snapshot.value?.stats);
export const samples = computed(() =>
  snapshot.value?.stats && state.selectedId
    ? (state.samples[state.selectedId] ?? [])
    : [],
);
export const isDark = computed(
  () =>
    state.theme === "dark" || (state.theme === "system" && state.systemDark),
);
export const results = computed(() =>
  state.selectedId ? (state.results[state.selectedId] ?? []) : [],
);
export const running = computed(() =>
  state.selectedId ? (state.running[state.selectedId] ?? false) : false,
);
export const script = computed({
  get: () =>
    state.selectedId
      ? (state.scripts[state.selectedId] ?? "PING\nINFO stats")
      : "",
  set: (value: string) => {
    if (state.selectedId) state.scripts[state.selectedId] = value;
  },
});

export function errorText(error: unknown): string {
  const message = error instanceof Error ? error.message : String(error);
  return message.replace(
    /^Error invoking remote method '[^']+': (?:Error: )?/,
    "",
  );
}

export function notice(
  message: string,
  type: "success" | "error" | "info" = "info",
): void {
  const id = crypto.randomUUID();
  state.notices.push({ id, message, type });
  if (state.notices.length > 4) state.notices.shift();
  setTimeout(
    () => {
      state.notices = state.notices.filter((item) => item.id !== id);
    },
    type === "error" ? 7000 : 3500,
  );
}

let pollTimer: ReturnType<typeof setTimeout> | undefined;
let pollToken = 0;

function applySnapshot(value: MonitoringSnapshot): void {
  state.snapshots[value.connectionId] = value;
  if (!value.stats) return;
  const history = (state.samples[value.connectionId] ??= []);
  const last = history.at(-1);
  const metric = (value: unknown): number | null =>
    typeof value === "number" && Number.isFinite(value) ? value : null;
  const total = metric(value.stats.total_commands);
  const elapsed = last ? value.timestamp - last.time : 0;
  history.push({
    time: value.timestamp,
    total,
    qps:
      last &&
      total !== null &&
      last.total !== null &&
      elapsed > 0 &&
      total >= last.total
        ? ((total - last.total) * 1000) / elapsed
        : null,
    memory: metric(value.stats.used_memory_bytes),
    latency: metric(value.stats.avg_command_latency_us),
    clients: metric(value.stats.connected_clients),
  });
  if (history.length > 60) history.shift();
}

export function refreshMonitoring(): void {
  clearTimeout(pollTimer);
  const token = ++pollToken;
  void (async () => {
    const profile = active.value;
    if (!profile || state.connections[profile.id]?.status !== "connected") {
      state.monitorLoading = false;
      return;
    }
    state.monitorLoading = true;
    try {
      const value = await window.desktop.monitoring(profile.id);
      if (token === pollToken && profile.id === state.selectedId)
        applySnapshot(value);
    } catch (error) {
      if (token === pollToken)
        state.snapshots[profile.id] = {
          connectionId: profile.id,
          timestamp: Date.now(),
          error: errorText(error),
        };
    } finally {
      if (token === pollToken) {
        state.monitorLoading = false;
        if (state.autoRefresh) pollTimer = setTimeout(refreshMonitoring, 2500);
      }
    }
  })();
}

export async function initialize(): Promise<void> {
  if (!window.desktop) {
    state.bootError = "请使用 npm run dev 或桌面客户端启动 MiniRedis Desktop。";
    return;
  }
  window.desktop.onEvent((event) => {
    if (event.type === "connection") {
      const previous = state.connections[event.state.id];
      state.connections[event.state.id] = event.state;
      if (
        event.state.status === "connected" &&
        previous?.status !== "connected"
      ) {
        delete state.samples[event.state.id];
        delete state.snapshots[event.state.id];
      }
      if (["disconnected", "error"].includes(event.state.status))
        state.running[event.state.id] = false;
      if (event.state.id === state.selectedId) refreshMonitoring();
    } else if (event.type === "execution") {
      state.running[event.connectionId] = event.running;
    } else if (event.type === "result") {
      const output = (state.results[event.result.connectionId] ??= []);
      output.push(event.result);
      if (output.length > 100) output.shift();
    } else if (event.type === "local") state.nodes = event.nodes;
    else if (event.type === "task") {
      state.task = event.task;
      if (event.task.status === "completed") notice("任务执行完成", "success");
      if (event.task.status === "failed")
        notice("任务执行失败，请查看日志", "error");
    } else if (event.type === "log") {
      state.logs.push(event.entry);
      if (state.logs.length > 500) state.logs.shift();
    }
  });
  try {
    const [workspace, nodes, task, logs] = await Promise.all([
      window.desktop.workspace(),
      window.desktop.localState(),
      window.desktop.taskState(),
      window.desktop.logs(),
    ]);
    state.connections = {
      ...Object.fromEntries(
        workspace.connections.map((connection) => [connection.id, connection]),
      ),
      ...state.connections,
    };
    state.running = {
      ...Object.fromEntries(workspace.running.map((id) => [id, true])),
      ...state.running,
    };
    state.workspace = workspace;
    state.profiles = workspace.profiles;
    state.selectedId = workspace.selectedId;
    state.theme = workspace.theme;
    state.nodes = nodes;
    state.task = task;
    state.logs = logs;
    state.ready = true;
  } catch (error) {
    state.bootError = errorText(error);
  }
}

const connectionAttempts = new Map<string, symbol>();
const executionAttempts = new Map<string, string>();

export async function connectProfile(id: string): Promise<void> {
  if (state.connections[id]?.status === "connecting") return;
  const attempt = Symbol(id);
  connectionAttempts.set(id, attempt);
  const current = () =>
    connectionAttempts.get(id) === attempt &&
    state.profiles.some((profile) => profile.id === id);
  state.connections[id] = { id, status: "connecting" };
  try {
    await window.desktop.connect(id);
    if (!current() || state.connections[id]?.status !== "connected") return;
    notice(
      "已连接到 " +
        (state.profiles.find((profile) => profile.id === id)?.name ?? "实例"),
      "success",
    );
  } catch (error) {
    if (!current() || state.connections[id]?.status === "disconnected") return;
    state.connections[id] = { id, status: "error", error: errorText(error) };
    notice(errorText(error), "error");
  } finally {
    if (connectionAttempts.get(id) === attempt) connectionAttempts.delete(id);
  }
}

export function forgetConnection(id: string): void {
  connectionAttempts.delete(id);
  executionAttempts.delete(id);
  state.profiles = state.profiles.filter((profile) => profile.id !== id);
  for (const collection of [
    state.connections,
    state.running,
    state.results,
    state.history,
    state.scripts,
    state.snapshots,
    state.samples,
  ])
    delete collection[id];
  if (state.selectedId === id) state.selectedId = state.profiles[0]?.id ?? null;
}

export async function execute(scriptText: string): Promise<void> {
  const id = state.selectedId;
  if (!id || state.running[id]) return;
  if (state.connections[id]?.status !== "connected") {
    notice("请先连接实例", "error");
    return;
  }
  const runId = crypto.randomUUID();
  executionAttempts.set(id, runId);
  state.running[id] = true;
  try {
    const outcome = await window.desktop.execute({
      connectionId: id,
      script: scriptText,
      runId,
    });
    const output = (state.results[id] ?? []).filter(
      (item) => item.runId === runId,
    );
    if (
      output.length &&
      !parseScript(scriptText).some(
        (parts) => parts[0]?.toUpperCase() === "AUTH",
      )
    ) {
      state.history[id] = [
        scriptText,
        ...(state.history[id] ?? []).filter((item) => item !== scriptText),
      ].slice(0, 20);
    }
    if (outcome.cancelled) notice("执行已取消", "info");
    if (id === state.selectedId) refreshMonitoring();
  } catch (error) {
    notice(errorText(error), "error");
  } finally {
    if (executionAttempts.get(id) === runId) {
      executionAttempts.delete(id);
      if (state.profiles.some((profile) => profile.id === id))
        state.running[id] = false;
      else delete state.running[id];
    }
  }
}

export async function setTheme(theme: Theme): Promise<void> {
  state.theme = theme;
  try {
    await window.desktop.preferences({ theme });
  } catch (error) {
    notice(errorText(error), "error");
  }
}

export function bytes(value: unknown): string {
  if (value === undefined || value === null) return "—";
  let number = Number(value);
  if (!Number.isFinite(number)) return "—";
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let index = 0;
  while (number >= 1024 && index < units.length - 1) {
    number /= 1024;
    index++;
  }
  return `${number.toLocaleString("en-US", { maximumFractionDigits: index ? 1 : 0 })} ${units[index]}`;
}

export function number(value: unknown, digits = 0): string {
  return value !== undefined && value !== null && Number.isFinite(Number(value))
    ? Number(value).toLocaleString("en-US", { maximumFractionDigits: digits })
    : "—";
}

matchMedia("(prefers-color-scheme: dark)").addEventListener(
  "change",
  (event) => {
    state.systemDark = event.matches;
  },
);
watch(
  isDark,
  (value) => {
    document.documentElement.dataset.theme = value ? "dark" : "light";
  },
  { immediate: true },
);
watch(
  () => state.selectedId,
  (id) => {
    refreshMonitoring();
    if (state.ready)
      void window.desktop
        .preferences({ selectedId: id })
        .catch((error) => notice(errorText(error), "error"));
  },
);
watch(() => state.autoRefresh, refreshMonitoring);
