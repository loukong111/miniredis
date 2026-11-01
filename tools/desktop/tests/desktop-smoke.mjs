import assert from "node:assert/strict";
import net from "node:net";
import { mkdtemp, mkdir, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
import { _electron as electron, expect } from "playwright/test";

const desktop = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const root = resolve(desktop, "../..");
const require = createRequire(import.meta.url);
const directory = await mkdtemp(join(tmpdir(), "miniredis-desktop-test-"));
const screenshots =
  process.env.MINIREDIS_SCREENSHOTS || join(directory, "screenshots");
await mkdir(screenshots, { recursive: true });
async function freePort() {
  const server = net.createServer();
  await new Promise((done) => server.listen(0, "127.0.0.1", done));
  const port = server.address().port;
  await new Promise((done) => server.close(done));
  return port;
}
// Reserve a consecutive pair for replication and another pair for monitoring.
async function pair() {
  for (let attempt = 0; attempt < 20; attempt++) {
    const port = await freePort();
    const server = net.createServer();
    try {
      await new Promise((done, reject) => {
        server.once("error", reject);
        server.listen(port + 1, "127.0.0.1", done);
      });
      await new Promise((done) => server.close(done));
      return port;
    } catch {
      server.close();
    }
  }
  throw new Error("Cannot find free port pair");
}
const port = await pair();
let statsPort = await pair();
while (Math.abs(port - statsPort) < 2) statsPort = await pair();
const args = [desktop];
if (process.env.MINIREDIS_TEST_NO_SANDBOX === "1") args.unshift("--no-sandbox");
const app = await electron.launch({
  executablePath: process.env.MINIREDIS_TEST_ELECTRON || require("electron"),
  args,
  env: {
    ...Object.fromEntries(
      Object.entries(process.env).filter(
        ([name]) => name !== "ELECTRON_RUN_AS_NODE",
      ),
    ),
    MINIREDIS_DESKTOP_DATA: directory,
    MINIREDIS_PROJECT_ROOT: root,
  },
  timeout: 30_000,
});
let page;
const errors = [];
let ownedPids = [];
try {
  page = await app.firstWindow();
  page.on("pageerror", (error) => errors.push(error.message));
  async function enter(locator, value) {
    await locator.click();
    await locator.press("ControlOrMeta+A");
    await locator.pressSequentially(value);
    await locator.press("Tab");
    await expect(locator).toHaveValue(value);
  }

  await expect(page.getByTestId("connect-current")).toBeVisible();
  await expect(page.locator(".boot-error")).toHaveCount(0);
  assert.equal(await page.evaluate(() => typeof window.require), "undefined");
  const binary = resolve(
    process.env.MINIREDIS_TEST_BINARY || join(root, "build/miniredis"),
  );
  const profiles = await page.evaluate(
    async ({ binary, port, statsPort }) => {
      const nodes = await window.desktop.startLocal({
        binary,
        mode: "replication",
        port,
        statsPort,
        password: "desktop-test-fixture",
        aof: true,
        ioThreads: 2,
        shards: 4,
        memoryMiB: 16,
        eviction: "noeviction",
      });
      const profiles = [];
      for (const [index, node] of nodes.entries())
        profiles.push(
          await window.desktop.saveConnection({
            name: index ? "本地副本" : "本地开发",
            host: "127.0.0.1",
            port: node.port,
            statsPort: node.statsPort,
            username: "",
            password: "desktop-test-fixture",
            color: index ? "#8b5cf6" : "#3b82f6",
            followRedirects: true,
          }),
        );
      await window.desktop.deleteConnection("local");
      await window.desktop.preferences({ selectedId: profiles[0].id });
      return { profiles, pids: nodes.map((node) => node.pid) };
    },
    { binary, port, statsPort },
  );
  ownedPids = profiles.pids;
  await page.reload();
  await page.getByTestId("connect-current").click();
  await expect(page.locator(".connection-badge")).toHaveText("已连接");
  await expect(
    page
      .locator(".workspace-header, .connection-header")
      .or(page.locator("header"))
      .filter({ hasText: "Primary" })
      .first(),
  ).toBeVisible();
  async function typeScript(script) {
    await page.locator(".monaco-editor").click({ position: { x: 170, y: 35 } });
    await page.keyboard.press("ControlOrMeta+A");
    await page.keyboard.insertText(script);
  }
  await typeScript(
    'SET demo:greeting "Hello MiniRedis" EX 120\nGET demo:greeting\nGET demo:greeting\nTTL demo:greeting',
  );
  await page.getByRole("button", { name: "运行全部", exact: true }).click();
  await expect(page.getByTestId("result-card")).toHaveCount(4);
  await expect(
    page.getByTestId("result-card").nth(1).locator("pre"),
  ).toHaveText("Hello MiniRedis");
  await expect(
    page.getByTestId("result-card").nth(2).locator("pre"),
  ).toHaveText("Hello MiniRedis");
  await page
    .getByTestId("result-card")
    .nth(1)
    .getByRole("button", { name: "复制响应" })
    .click();
  assert.equal(
    await app.evaluate(({ clipboard }) => clipboard.readText()),
    "Hello MiniRedis",
  );
  await expect(page.locator(".toast-stack > *")).toHaveCount(0, {
    timeout: 10_000,
  });
  await page.screenshot({ path: join(screenshots, "desktop-workbench.png") });
  await typeScript("AUTH desktop-test-fixture\nPING");
  await page.getByRole("button", { name: "运行全部", exact: true }).click();
  await expect(page.getByTestId("result-card")).toHaveCount(6);
  assert.equal(
    (await page.getByTestId("results").textContent()).includes(
      "desktop-test-fixture",
    ),
    false,
  );
  await page.getByRole("button", { name: "命令历史" }).click();
  assert.equal(
    (await page.locator(".n-dropdown-menu").textContent()).includes(
      "desktop-test-fixture",
    ),
    false,
  );
  await page.keyboard.press("Escape");
  await typeScript(
    'SET demo:greeting \"Hello MiniRedis\" EX 120\nGET demo:greeting\nGET demo:greeting\nTTL demo:greeting',
  );
  await page.getByTestId("nav-monitoring").click();
  await expect(page.locator(".metric-value").first()).not.toHaveText("—");
  // Gather real time samples while issuing real requests, without seeded chart data.
  await page.evaluate(async (id) => {
    for (let index = 0; index < 3; index++) {
      await window.desktop.execute({
        connectionId: id,
        script: "INCR demo:counter\nGET demo:counter",
        runId: `sample-${index}`,
      });
      await new Promise((done) => setTimeout(done, 2700));
    }
  }, profiles.profiles[0].id);
  await page.screenshot({ path: join(screenshots, "desktop-monitoring.png") });
  await page.getByRole("button", { name: "Prometheus" }).click();
  await expect(page.locator(".raw-output")).toContainText("miniredis_");
  await page.locator(".n-drawer-header__close").click();
  await expect(page.getByTestId("nav-monitoring")).toHaveClass(/selected/);
  await page.getByTestId("nav-topology").click();
  await expect(page.locator(".topology-page")).toContainText("Primary");
  await page.screenshot({ path: join(screenshots, "desktop-topology.png") });
  await page.getByTestId("nav-workbench").click();
  await page.getByTestId("theme-toggle").click();
  await page.waitForTimeout(400);
  await page.screenshot({ path: join(screenshots, "desktop-dark.png") });
  await page.getByTestId("theme-toggle").click();
  await page.getByTestId("open-local").click();
  await expect(page.locator(".n-drawer")).toContainText(String(port));
  await page.waitForTimeout(450);
  await page.screenshot({ path: join(screenshots, "desktop-local.png") });
  // Editing next-start credentials must not change an owned node's credentials.
  await page
    .getByPlaceholder("设置该组节点的共享密码")
    .fill("different-next-start-password");
  await page
    .getByRole("button", { name: "连接此节点", exact: true })
    .first()
    .click();
  await expect(page.locator(".workspace-location")).toContainText(
    "本地 · Primary",
  );
  await expect(page.locator(".connection-badge")).toHaveText("已连接");

  await page.locator(".n-drawer-header__close").click();
  await page.getByRole("button", { name: "任务中心", exact: true }).click();
  await page.getByTestId("task-binary").locator("input").fill(binary);
  for (const [field, value] of [
    ["requests", "1000"],
    ["clients", "2"],
    ["payload", "64"],
    ["runs", "1"],
  ]) {
    await page.getByTestId(`task-${field}`).locator("input").fill(value);
    await page.getByTestId(`task-${field}`).locator("input").blur();
  }
  await page.getByRole("button", { name: "运行任务", exact: true }).click();
  await expect(page.locator(".task-status-card")).toContainText("已完成", {
    timeout: 30_000,
  });
  await page.locator(".n-drawer-header__close").click();
  await expect(page.locator(".log-dock")).toContainText("任务开始");
  await page.getByTestId("add-connection").click();
  await enter(page.getByTestId("connection-name").locator("input"), "手动连接");
  await enter(
    page.getByTestId("connection-port").locator("input"),
    String(port),
  );
  await enter(
    page.getByTestId("connection-stats").locator("input"),
    String(statsPort),
  );
  await enter(
    page.getByTestId("connection-password").locator("input"),
    "desktop-test-fixture",
  );
  await page.getByTestId("connection-save").click();
  await expect(page.locator(".workspace-location")).toContainText("手动连接");
  await expect(page.locator(".connection-badge")).toHaveText("已连接");

  assert.deepEqual(errors, [], "renderer errors");
  const persisted = await readFile(join(directory, "workspace.json"), "utf8");
  assert.equal(persisted.includes("desktop-test-fixture"), false);
  console.log(
    "Desktop smoke passed: real authentication, editor, duplicate responses, clipboard, monitoring, topology, themes, local instances, task completion and connection forms.",
  );
} catch (error) {
  console.error("Renderer errors:", errors);
  if (page) {
    await page
      .screenshot({ path: "/tmp/miniredis-desktop-failure.png" })
      .catch(() => {});
    console.error((await page.locator("body").innerText()).slice(0, 5000));
  }
  throw error;
} finally {
  await app.close();
  for (const pid of ownedPids) {
    let running = false;
    try {
      process.kill(pid, 0);
      running = true;
    } catch {
      /* Exited */
    }
    assert.equal(
      running,
      false,
      `owned server ${pid} should exit with the desktop app`,
    );
  }
  await rm(directory, { recursive: true, force: true });
}
