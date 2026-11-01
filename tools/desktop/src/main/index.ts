import {
  app,
  BrowserWindow,
  clipboard,
  dialog,
  ipcMain,
  net as electronNet,
  protocol,
  safeStorage,
} from "electron";
import { existsSync } from "node:fs";
import { writeFile } from "node:fs/promises";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import type {
  ConnectionInput,
  DesktopEvent,
  LocalConfig,
  TaskInput,
  Theme,
  Workspace,
} from "../shared/contracts";
import { LocalManager, LogBook, TaskManager } from "./processes";
import { asError, fetchEndpoint, monitoring, RedisSession } from "./redis";
import { WorkspaceStore } from "./workspace";

protocol.registerSchemesAsPrivileged([
  {
    scheme: "app",
    privileges: {
      standard: true,
      secure: true,
      supportFetchAPI: true,
      corsEnabled: true,
      allowServiceWorkers: false,
    },
  },
]);
if (process.env.MINIREDIS_DESKTOP_DATA)
  app.setPath("userData", resolve(process.env.MINIREDIS_DESKTOP_DATA));
app.setName("MiniRedis Desktop");

const mainDirectory = dirname(fileURLToPath(import.meta.url));
const rendererDirectory = resolve(mainDirectory, "../renderer");
const candidates = [
  process.env.MINIREDIS_PROJECT_ROOT,
  resolve(app.getAppPath(), "../.."),
  process.cwd(),
].filter((value): value is string => Boolean(value));
const projectRoot =
  candidates.find(
    (path) =>
      existsSync(join(path, "src/server/server.cpp")) &&
      existsSync(join(path, "CMakeLists.txt")),
  ) ?? null;
const binary = projectRoot
  ? (["build/miniredis", "build-review/miniredis", "build-release/miniredis"]
      .map((path) => join(projectRoot, path))
      .find(existsSync) ?? "")
  : "";
const sessions = new Map<string, RedisSession>();
let window: BrowserWindow | null = null;
let store: WorkspaceStore;
let local: LocalManager;
let tasks: TaskManager;
let logs: LogBook;
let shuttingDown = false;

function emit(event: DesktopEvent): void {
  if (window && !window.isDestroyed())
    window.webContents.send("desktop:event", event);
}

function session(id: string): RedisSession {
  const connection = sessions.get(id);
  if (!connection) throw new Error("请先连接实例");
  return connection;
}

function registerHandlers(): void {
  const handlers: Record<string, (input: any) => unknown> = {
    workspace: (): Workspace => ({
      profiles: store.list(),
      theme: store.theme,
      selectedId: store.selectedId,
      binary,
      projectRoot,
      platform: process.platform,
      passwordStorage: secretsAvailable() ? "encrypted" : "session",
      version: app.getVersion(),
    }),
    saveConnection: async (input: ConnectionInput) => {
      const profile = await store.save(input);
      sessions.get(profile.id)?.disconnect();
      sessions.delete(profile.id);
      return profile;
    },
    deleteConnection: async (id: string) => {
      await store.remove(id);
      sessions.get(id)?.disconnect();
      sessions.delete(id);
    },
    preferences: (input: { theme?: Theme; selectedId?: string | null }) =>
      store.preferences(input),
    connect: async (id: string) => {
      const profile = store.get(id);
      const password = await store.password(id);
      sessions.get(id)?.disconnect();
      const connection = new RedisSession(
        profile,
        profile.username,
        password,
        emit,
      );
      sessions.set(id, connection);
      await connection.connect();
    },
    disconnect: (id: string) => {
      store.get(id);
      sessions.get(id)?.disconnect();
    },
    execute: (input: {
      connectionId: string;
      script: string;
      runId: string;
    }) => {
      if (!input || typeof input.runId !== "string" || input.runId.length > 100)
        throw new Error("执行请求无效");
      return session(input.connectionId).execute(input.script, input.runId);
    },
    cancel: (id: string) => session(id).cancel(),
    monitoring: (id: string) => monitoring(store.get(id)),
    topology: (id: string) => session(id).topology(),
    metrics: async (id: string) => {
      const response = await fetchEndpoint(store.get(id), "/metrics");
      if (response.status !== 200)
        throw new Error(`指标接口返回 HTTP ${response.status}`);
      return response.body;
    },
    pickBinary: async () => {
      const selection = await dialog.showOpenDialog(window!, {
        title: "选择 MiniRedis 服务端程序",
        properties: ["openFile"],
      });
      return selection.canceled ? null : (selection.filePaths[0] ?? null);
    },
    startLocal: (config: LocalConfig) => local.start(config),
    stopLocal: () => local.stop(),
    connectLocal: async (id: string) => {
      const { node, password } = local.connection(id);
      const name = `本地 · ${node.label} :${node.port}`;
      const existing = store
        .list()
        .find(
          (profile) =>
            profile.name === name &&
            profile.host === "127.0.0.1" &&
            profile.port === node.port,
        );
      const profile = await store.save({
        id: existing?.id,
        name,
        host: "127.0.0.1",
        port: node.port,
        statsPort: node.statsPort,
        username: "",
        password,
        forgetPassword: !password,
        color: node.label.includes("Replica") ? "#8b5cf6" : "#3b82f6",
        followRedirects: true,
      });
      sessions.get(profile.id)?.disconnect();
      const connection = new RedisSession(profile, "", password, emit);
      sessions.set(profile.id, connection);
      await connection.connect();
      return profile;
    },
    copyText: (text: string) => {
      if (
        typeof text !== "string" ||
        Buffer.byteLength(text) > 64 * 1024 * 1024
      )
        throw new Error("复制内容过大或格式无效");
      clipboard.writeText(text);
    },
    stopNode: (id: string) => local.stopNode(id),
    localState: () => local.list(),
    runTask: (input: TaskInput) => tasks.run(input),
    stopTask: () => tasks.stop(),
    taskState: () => tasks.current(),
    logs: () => logs.list(),
    exportText: async (input: { filename: string; content: string }) => {
      if (
        !input ||
        typeof input.content !== "string" ||
        Buffer.byteLength(input.content) > 64 * 1024 * 1024
      )
        throw new Error("导出内容过大或格式无效");
      const filename =
        typeof input.filename === "string"
          ? input.filename.replace(/[/\\]/g, "_").slice(0, 100)
          : "miniredis.txt";
      const selection = await dialog.showSaveDialog(window!, {
        title: "导出文本",
        defaultPath: filename,
        filters: [{ name: "文本文件", extensions: ["txt", "log", "json"] }],
      });
      if (selection.canceled || !selection.filePath) return false;
      await writeFile(selection.filePath, input.content, { mode: 0o600 });
      return true;
    },
  };
  for (const [name, handler] of Object.entries(handlers)) {
    ipcMain.handle(`desktop:${name}`, async (event, input: unknown) => {
      if (
        !window ||
        event.sender !== window.webContents ||
        event.senderFrame?.url !== window.webContents.getURL()
      )
        throw new Error("调用来源无效");
      try {
        return await handler(input);
      } catch (error) {
        throw new Error(asError(error).message);
      }
    });
  }
}

function secretsAvailable(): boolean {
  // Linux's basic_text provider uses a hardcoded key; keep credentials in memory instead.
  return (
    encryptionAvailable &&
    (process.platform !== "linux" ||
      !["basic_text", "unknown"].includes(
        safeStorage.getSelectedStorageBackend(),
      ))
  );
}
let encryptionAvailable = false;

async function createWindow(): Promise<void> {
  window = new BrowserWindow({
    width: 1440,
    height: 940,
    minWidth: 1060,
    minHeight: 700,
    title: "MiniRedis Desktop",
    icon: join(app.getAppPath(), "resources/icon.png"),
    backgroundColor: "#f6f8fb",
    show: false,
    autoHideMenuBar: true,
    webPreferences: {
      preload: join(mainDirectory, "../preload/index.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      webSecurity: true,
    },
  });
  window.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
  window.webContents.on("will-navigate", (event, url) => {
    if (url !== window?.webContents.getURL()) event.preventDefault();
  });
  window.webContents.session.setPermissionRequestHandler(
    (_webContents, _permission, callback) => callback(false),
  );
  window.webContents.on("render-process-gone", (_event, detail) =>
    logs.add("桌面端", `界面进程退出：${detail.reason}`),
  );
  window.once("ready-to-show", () => window?.show());
  if (process.env.ELECTRON_RENDERER_URL && !app.isPackaged)
    await window.loadURL(process.env.ELECTRON_RENDERER_URL);
  else await window.loadURL("app://desktop/index.html");
}

app.whenReady().then(async () => {
  try {
    protocol.handle("app", (request) => {
      const url = new URL(request.url);
      const filename = resolve(
        rendererDirectory,
        `.${decodeURIComponent(url.pathname === "/" ? "/index.html" : url.pathname)}`,
      );
      if (
        url.hostname !== "desktop" ||
        !filename.startsWith(rendererDirectory + sep)
      )
        return new Response("Not found", { status: 404 });
      return electronNet.fetch(pathToFileURL(filename).toString());
    });
    encryptionAvailable = await safeStorage
      .isAsyncEncryptionAvailable()
      .catch(() => false);
    store = new WorkspaceStore(app.getPath("userData"), {
      available: secretsAvailable,
      encrypt: async (text) =>
        (await safeStorage.encryptStringAsync(text)).toString("base64"),
      decrypt: async (text) =>
        (await safeStorage.decryptStringAsync(Buffer.from(text, "base64")))
          .result,
    });
    await store.load();
    logs = new LogBook(emit);
    local = new LocalManager(app.getPath("userData"), emit, logs);
    tasks = new TaskManager(projectRoot, app.getPath("userData"), emit, logs);
    registerHandlers();
    await createWindow();
    app.on("activate", () => {
      if (BrowserWindow.getAllWindows().length === 0) void createWindow();
    });
  } catch (error) {
    dialog.showErrorBox("MiniRedis Desktop 启动失败", asError(error).message);
    app.exit(1);
  }
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});
app.on("before-quit", (event) => {
  if (shuttingDown) return;
  event.preventDefault();
  shuttingDown = true;
  for (const connection of sessions.values()) connection.disconnect();
  void Promise.allSettled([local?.stop(), tasks?.stop()]).then(() =>
    app.exit(),
  );
});
