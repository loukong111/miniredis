import http from "node:http";
import net from "node:net";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import {
  harness,
  ports,
  query,
  redisFixture,
  binary,
  assert,
  expect,
  delay,
} from "./desktop-harness.mjs";
import { RespDecoder } from "../src/main/protocol.ts";

const qa = await harness();
const { page, app, run } = qa;
const cleanup = [];
const password = "qa-only-password";
let primary;
let unauthenticated;
let acl;
const lastCard = () => page.getByTestId("result-card").last();
const formItem = (label) =>
  page
    .locator(".n-form-item")
    .filter({ has: page.getByText(label, { exact: true }) });

async function field(label, value) {
  const input = formItem(label).locator("input");
  await input.fill(String(value));
  await input.blur();
}
async function choose(label, option) {
  await formItem(label).locator(".n-base-selection").click();
  await page
    .locator(".n-base-select-option")
    .filter({ hasText: option })
    .click();
}
async function closeTools() {
  const close = page
    .locator(".n-drawer-header__close")
    .filter({ visible: true });
  if (await close.count()) {
    try {
      await close.last().click({ timeout: 1200 });
    } catch (error) {
      if (await close.count()) throw error;
    }
  }
  await expect(page.locator(".n-drawer").filter({ visible: true })).toHaveCount(
    0,
  );
}

async function select(name, connect = true) {
  await closeTools();
  await page
    .getByRole("button", { name: "选择连接 " + name, exact: true })
    .click();
  if (connect && (await page.getByTestId("connect-current").isVisible()))
    await page.getByTestId("connect-current").click();
  if (connect)
    await expect(page.locator(".connection-badge")).toHaveText("已连接");
}
async function fillProfile(values) {
  for (const [key, value] of Object.entries(values)) {
    if (["name", "host", "port", "stats", "password"].includes(key)) {
      await page
        .getByTestId("connection-" + key)
        .locator("input")
        .fill(String(value));
    } else if (key === "username") await field("ACL 用户名（可选）", value);
    else if (key === "follow") {
      const toggle = page.locator(".connection-dialog .n-switch");
      if (((await toggle.getAttribute("aria-checked")) === "true") !== value)
        await toggle.click();
    } else if (key === "forget" && value)
      await page.getByRole("checkbox", { name: "移除已保存密码" }).check();
  }
}
async function addProfile(values, connect = true) {
  await page.getByTestId("add-connection").click();
  await fillProfile(values);
  await page
    .getByRole("button", {
      name: connect ? "保存并连接" : "仅保存",
      exact: true,
    })
    .click();
  await expect(page.locator(".connection-dialog")).not.toBeVisible();
  if (connect)
    await expect(page.locator(".connection-badge")).toHaveText("已连接");
}
async function editProfile(name, values, connect = true) {
  await page
    .getByRole("button", { name: "管理连接 " + name, exact: true })
    .click();
  await page.getByText("编辑连接", { exact: true }).click();
  await fillProfile(values);
  await page
    .getByRole("button", {
      name: connect ? "保存并连接" : "仅保存",
      exact: true,
    })
    .click();
  await expect(page.locator(".connection-dialog")).not.toBeVisible();
  if (connect)
    await expect(page.locator(".connection-badge")).toHaveText("已连接");
}
async function script(text, action = "all") {
  await page.getByTestId("nav-workbench").click();
  await page.locator(".monaco-editor").click({ position: { x: 160, y: 30 } });
  await page.keyboard.press("ControlOrMeta+A");
  await page.keyboard.insertText(text);
  if (action === "all")
    await page.getByRole("button", { name: "运行全部", exact: true }).click();
  else if (action === "current") await page.getByTestId("run-command").click();
  else if (action === "shortcut")
    await page.keyboard.press("ControlOrMeta+Shift+Enter");
  else if (action === "selection") {
    await page.keyboard.press("ControlOrMeta+A");
    await page.keyboard.press("ControlOrMeta+Enter");
  }
}
async function clearResults() {
  await page.getByTestId("nav-workbench").click();
  const clear = page.getByRole("button", { name: "清空结果", exact: true });
  if (await clear.isEnabled()) await clear.click();
  await expect(page.getByTestId("result-card")).toHaveCount(0);
}
async function waitWritable(port) {
  await expect
    .poll(
      async () =>
        (await query(port, ["INFO", "replication"], "qa-local-password")).value,
      { message: "主节点应取得多数派写入许可", timeout: 15000 },
    )
    .toMatch(/^failover_writes_allowed:1\r?$/m);
}
async function snapshot(key) {
  await page.screenshot({ path: join(qa.artifacts, key + ".png") });
}
async function exportDialog(filePath, canceled = false) {
  // Leave the response tooltip before clicking actions in the results header.
  await page.mouse.move(0, 0);
  await app.evaluate(
    ({ dialog }, result) => {
      globalThis.qaOriginalSaveDialog ??= dialog.showSaveDialog;
      dialog.showSaveDialog = async () => result;
    },
    { filePath, canceled },
  );
}

// Fault injection is confined to explicit test servers. Normal workflows use the C++ server.
async function faults() {
  const addresses = await ports();
  const state = {
    stats: "ok",
    health: 200,
    ready: 200,
    metrics: "ok",
    metricDelay: 0,
    statsCalls: 0,
    received: [],
    redirect: null,
    sockets: new Set(),
  };
  const resp = net.createServer((socket) => {
    state.sockets.add(socket);
    socket.on("error", () => {});
    socket.on("close", () => state.sockets.delete(socket));
    const decoder = new RespDecoder();
    socket.on("data", (chunk) => {
      for (const frame of decoder.feed(chunk)) {
        const parts = frame.value.map((value) => value.value);
        state.received.push(parts);
        const bulk = (value) => `$${Buffer.byteLength(value)}\r\n${value}\r\n`;
        const command = parts[0].toUpperCase();
        if (command === "PING") socket.write("+PONG\r\n");
        else if (command === "AUTH") socket.write("+OK\r\n");
        else if (command === "INFO")
          socket.write(bulk("role:master\r\nmaster_repl_offset:12\r\n"));
        else if (command === "CLUSTER")
          socket.write(
            bulk(
              `fixture 127.0.0.1:${addresses.port} myself,master - 0 0 1 connected 0-16383\n`,
            ),
          );
        else if (parts[1] === "qa:block") {
          /* Deliberately wait until cancellation or timeout. */
        } else if (state.redirect && command === "GET")
          socket.write(
            `-${state.redirect.kind} 42 127.0.0.1:${state.redirect.port}\r\n`,
          );
        else if (command === "ASKING") socket.write("+OK\r\n");
        else if (command === "GET") socket.write(bulk("fixture-value"));
        else socket.write("+OK\r\n");
      }
    });
  });
  const monitor = http.createServer(async (request, response) => {
    if (request.url === "/healthz" || request.url === "/readyz") {
      response.writeHead(
        request.url === "/healthz" ? state.health : state.ready,
      );
      response.end("status");
      return;
    }
    if (request.url === "/metrics") {
      await delay(state.metricDelay);
      response.writeHead(state.metrics === "ok" ? 200 : 503);
      response.end(
        state.metrics === "ok"
          ? "# fixture\nminiredis_qa_fixture 1\n"
          : "not available",
      );
      return;
    }
    state.statsCalls++;
    response.setHeader("content-type", "application/json");
    if (state.stats === "bad-json") response.end("not json");
    else if (state.stats === "oversized")
      response.end("x".repeat(1024 * 1024 + 1));
    else if (state.stats === "error") {
      response.writeHead(500);
      response.end("error");
    } else
      response.end(
        JSON.stringify({
          total_commands: state.statsCalls * 10,
          key_count: 42,
          used_memory_bytes: 1024,
          hit_rate: 0.5,
          avg_command_latency_us: 12,
          connected_clients: 1,
          maxmemory_bytes: 0,
          replication_connected_replicas: 0,
          replication_configured_replicas: 0,
          snapshot_running: false,
          failover_enabled: false,
        }),
      );
  });
  await new Promise((done) => resp.listen(addresses.port, "127.0.0.1", done));
  await new Promise((done) =>
    monitor.listen(addresses.statsPort, "127.0.0.1", done),
  );
  cleanup.push(async () => {
    for (const socket of state.sockets) socket.destroy();
    await Promise.all([
      new Promise((done) => resp.close(done)),
      new Promise((done) => monitor.close(done)),
    ]);
  });
  return { ...addresses, state };
}

try {
  primary = await redisFixture(join(qa.directory, "primary"), { password });
  cleanup.push(primary.stop);
  unauthenticated = await redisFixture(join(qa.directory, "plain"));
  cleanup.push(unauthenticated.stop);
  acl = await redisFixture(join(qa.directory, "acl"), {
    args: ["--acl-user", "auditor:qa-audit:readonly"],
  });
  cleanup.push(acl.stop);
  const fault = await faults();
  const target = await faults();

  await run("offline", "离线页面、禁用执行与命令参考", async () => {
    await expect(page.getByTestId("run-command")).toBeDisabled();
    for (const name of ["monitoring", "topology", "workbench"])
      await page.getByTestId("nav-" + name).click();
    await page.getByRole("button", { name: "命令参考", exact: true }).click();
    await expect(page.locator(".command-help")).toContainText("GET key");
    await page.getByRole("button", { name: "命令参考", exact: true }).click();
    assert.equal(await page.evaluate(() => typeof window.require), "undefined");
  });
  await run("profile-validation", "空名称、无效主机和保存取消", async () => {
    await page.getByTestId("add-connection").click();
    await page.getByTestId("connection-save").click();
    await expect(page.locator(".toast-stack")).toContainText("连接名称");
    await fillProfile({ name: "invalid", host: "http://invalid/" });
    await page.getByTestId("connection-save").click();
    await expect(page.locator(".toast-stack")).toContainText("主机");
    await page
      .locator(".connection-dialog")
      .getByRole("button", { name: "关闭", exact: true })
      .click();
    assert.equal(
      (await page.evaluate(() => window.desktop.workspace())).profiles.length,
      1,
    );
  });
  await run("auth-failure", "仅保存、错误密码连接失败", async () => {
    await addProfile(
      {
        name: "QA-primary",
        host: "127.0.0.1",
        port: primary.port,
        stats: primary.statsPort,
        password: "wrong-password",
      },
      false,
    );
    await page.getByTestId("connect-current").click();
    await expect(page.locator(".connection-badge")).toHaveText("连接失败");
    await expect(page.locator(".toast-stack")).toContainText("认证失败");
  });
  await run(
    "auth-correct-edit",
    "编辑修正密码、空密码保留已存密码",
    async () => {
      await editProfile("QA-primary", { password });
      await editProfile("QA-primary", { name: "QA-primary" });
      await expect(page.locator(".workspace-location")).toContainText(
        "QA-primary",
      );
    },
  );
  await run("search", "搜索匹配与无结果", async () => {
    const search = page.getByRole("textbox", { name: "搜索连接" });
    await search.fill("QA-primary");
    await expect(page.locator(".connection-item")).toHaveCount(1);
    await search.fill("no-such-profile");
    await expect(page.locator(".connection-item")).toHaveCount(0);
    await search.fill("");
  });
  await run("plain-acl", "无密码连接、具名只读 ACL 与写入拒绝", async () => {
    await addProfile({
      name: "QA-plain",
      port: unauthenticated.port,
      stats: unauthenticated.statsPort,
    });
    await clearResults();
    await script("PING");
    await expect(lastCard().locator("pre")).toHaveText("PONG");
    await addProfile({
      name: "QA-auditor",
      port: acl.port,
      stats: acl.statsPort,
      username: "auditor",
      password: "qa-audit",
    });
    await script("GET qa:missing\nSET qa:denied value");
    await expect(page.getByTestId("result-card")).toHaveCount(2);
    await expect(lastCard()).toHaveClass(/response-error/);
    await select("QA-primary");
  });
  await run(
    "responses",
    "Unicode、空值、数组、整数、Redis 错误与 HTML 文本",
    async () => {
      await clearResults();
      await script(
        'SET qa:greeting "你好 世界"\nGET qa:greeting\nGET qa:missing\nINCR qa:counter\nMGET qa:greeting qa:missing qa:counter\nNOT_A_COMMAND\nSET qa:html "<img src=x onerror=alert(1)>"\nGET qa:html',
      );
      await expect(page.getByTestId("result-card")).toHaveCount(8);
      await expect(
        page.getByTestId("result-card").nth(1).locator("pre"),
      ).toHaveText("你好 世界");
      await expect(
        page.getByTestId("result-card").nth(2).locator("pre"),
      ).toHaveText("(nil)");
      await expect(
        page.getByTestId("result-card").nth(4).locator("pre"),
      ).toContainText("1) 你好 世界");
      await expect(page.getByTestId("result-card").nth(5)).toHaveClass(
        /response-error/,
      );
      await expect(lastCard().locator("pre")).toHaveText(
        "<img src=x onerror=alert(1)>",
      );
      await expect(lastCard().locator("img")).toHaveCount(0);
    },
  );
  await run(
    "syntax-validation",
    "整段脚本预校验，不部分执行错误脚本",
    async () => {
      await clearResults();
      await script('SET qa:must-not-exist yes\nSET broken "unfinished');
      await expect(page.locator(".toast-stack")).toContainText("引号未闭合");
      await expect(page.getByTestId("result-card")).toHaveCount(0);
      assert.deepEqual(
        await query(primary.port, ["GET", "qa:must-not-exist"], password),
        { type: "null" },
      );
      await script("# only comments");
      await expect(page.locator(".toast-stack")).toContainText(
        "没有可执行命令",
      );
    },
  );
  await run("keyboard", "当前行、选区和两种键盘快捷键", async () => {
    await clearResults();
    await script("PING\nGET qa:greeting", "current");
    await expect(page.getByTestId("result-card")).toHaveCount(1);
    await expect(lastCard().locator("pre")).toHaveText("你好 世界");
    await script("PING\nPING", "selection");
    await expect(page.getByTestId("result-card")).toHaveCount(3);
    await script("PING\nPING", "shortcut");
    await expect(page.getByTestId("result-card")).toHaveCount(5);
  });
  await run("templates-history", "全部模板、命令历史和 AUTH 脱敏", async () => {
    for (const name of [
      "基础读写与 TTL",
      "计数器",
      "持久化状态",
      "复制状态",
      "集群拓扑",
      "慢日志",
    ]) {
      await page.getByRole("button", { name: "模板", exact: true }).click();
      await page.getByText(name, { exact: true }).click();
      await page.getByRole("button", { name: "运行全部", exact: true }).click();
      await expect(
        page.getByRole("button", { name: "取消", exact: true }),
      ).toHaveCount(0);
    }
    await script(`AUTH ${password}\nPING`);
    await expect(lastCard().locator("pre")).toHaveText("PONG");
    assert.equal(
      (await page.getByTestId("results").textContent()).includes(password),
      false,
    );
    await page.getByRole("button", { name: "命令历史", exact: true }).click();
    assert.equal(
      (await page.locator(".n-dropdown-menu").textContent()).includes(password),
      false,
    );
    await page.locator(".n-dropdown-option").first().click();
  });
  await run(
    "long-result-export",
    "大响应展开、复制、导出成功与取消",
    async () => {
      await clearResults();
      await script(`SET qa:long "${"x".repeat(25000)}"\nGET qa:long`);
      await expect(page.getByTestId("result-card")).toHaveCount(2);
      await expect(lastCard().locator("pre")).toContainText("响应较长");
      await lastCard()
        .getByRole("button", { name: "展开响应", exact: false })
        .click();
      await expect(lastCard().locator("pre")).toHaveText("x".repeat(25000));
      await lastCard()
        .getByRole("button", { name: "收起", exact: false })
        .click();
      await lastCard()
        .getByRole("button", { name: "复制响应", exact: true })
        .click();
      assert.equal(
        (await app.evaluate(({ clipboard }) => clipboard.readText())).length,
        25000,
      );
      const output = join(qa.directory, "results.txt");
      await exportDialog(output);
      await page.getByRole("button", { name: "导出结果", exact: true }).click();
      await expect
        .poll(
          async () => (await readFile(output, "utf8").catch(() => "")).length,
        )
        .toBeGreaterThan(25000);
      await exportDialog(undefined, true);
      await page.getByRole("button", { name: "导出结果", exact: true }).click();
    },
  );
  await run(
    "profile-isolation",
    "连接之间的编辑内容、响应与历史隔离",
    async () => {
      await select("QA-plain");
      await clearResults();
      await script("PING");
      await expect(page.getByTestId("result-card")).toHaveCount(1);
      await select("QA-primary");
      await expect(page.getByTestId("result-card")).toHaveCount(2);
      await clearResults();
      await script("PING\n".repeat(105));
      await expect(page.getByTestId("result-card")).toHaveCount(100);
    },
  );
  await run("reload-connected", "刷新页面后恢复真实连接状态", async () => {
    await page.reload();
    await expect(page.locator(".connection-badge")).toHaveText("已连接");
    await script("PING");
    await expect(lastCard().locator("pre")).toHaveText("PONG");
  });
  // Restore a usable connection after the reload regression check, so other cases remain independent.
  await select("QA-primary");
  await run(
    "fault-profile",
    "建立受控故障服务连接，用于错误分支验证",
    async () => {
      await addProfile({
        name: "QA-faults",
        port: fault.port,
        stats: fault.statsPort,
      });
      await page.getByTestId("nav-monitoring").click();
      await expect(page.locator(".metric-value").nth(1)).toContainText("42");
    },
  );
  await run("auto-refresh", "自动刷新开启、关闭和手动刷新", async () => {
    const toggle = page.locator(".auto-refresh .n-switch");
    await toggle.click();
    await delay(500);
    const before = fault.state.statsCalls;
    await delay(2900);
    assert.equal(fault.state.statsCalls, before);
    await page.getByRole("button", { name: "刷新监控", exact: true }).click();
    await expect.poll(() => fault.state.statsCalls).toBeGreaterThan(before);
    await toggle.click();
    const resumed = fault.state.statsCalls;
    await expect
      .poll(() => fault.state.statsCalls, { timeout: 5000 })
      .toBeGreaterThan(resumed);
  });
  await run("health-failure", "存活异常、未就绪和监控接口错误", async () => {
    fault.state.health = 500;
    fault.state.ready = 503;
    await page.getByRole("button", { name: "刷新监控", exact: true }).click();
    await expect(page.locator(".monitoring-page")).toContainText("未就绪");
    fault.state.stats = "bad-json";
    await page.getByRole("button", { name: "刷新监控", exact: true }).click();
    await expect(page.locator(".monitoring-page .inline-alert")).toContainText(
      "无法读取",
    );
    await expect(page.locator(".metric-value").first()).toHaveText("—cmd/s");
  });
  await run("monitor-recover", "超大监控响应拒绝和错误后恢复", async () => {
    fault.state.stats = "oversized";
    await page.getByRole("button", { name: "刷新监控", exact: true }).click();
    await expect(page.locator(".monitoring-page .inline-alert")).toContainText(
      "1 MiB",
    );
    fault.state.stats = "ok";
    fault.state.health = 200;
    fault.state.ready = 200;
    await page.getByRole("button", { name: "刷新监控", exact: true }).click();
    await expect(page.locator(".metric-value").nth(1)).toContainText("42");
  });
  await run("metrics-errors", "Prometheus 失败、关闭和重试恢复", async () => {
    fault.state.metrics = "error";
    await page.getByRole("button", { name: "Prometheus", exact: true }).click();
    await expect(page.locator(".n-drawer")).toContainText("503");
    await closeTools();
    fault.state.metrics = "ok";
    await page.getByRole("button", { name: "Prometheus", exact: true }).click();
    await expect(page.locator(".raw-output")).toContainText(
      "miniredis_qa_fixture",
    );
    await closeTools();
  });
  await run(
    "metrics-scope",
    "读取 Prometheus 时切换连接不显示旧实例指标",
    async () => {
      fault.state.metricDelay = 1900;
      await page
        .getByRole("button", { name: "Prometheus", exact: true })
        .click();
      await closeTools();
      await select("QA-primary");
      await page.getByTestId("nav-monitoring").click();
      await page
        .getByRole("button", { name: "Prometheus", exact: true })
        .click();
      await expect(page.locator(".raw-output")).toContainText(
        "miniredis_total_commands",
      );
      await delay(2200);
      await expect(page.locator(".raw-output")).not.toContainText(
        "miniredis_qa_fixture",
      );
      await closeTools();
    },
  );
  await run("redirects", "MOVED、ASK 和禁止自动重定向", async () => {
    await select("QA-faults");
    await clearResults();
    for (const kind of ["MOVED", "ASK"]) {
      fault.state.redirect = { kind, port: target.port };
      await script(`GET qa:${kind}`);
      await expect(lastCard().locator("pre")).toHaveText("fixture-value");
      await expect(lastCard().locator(".redirect-note")).toContainText(kind);
    }
    assert.equal(
      target.state.received.some((parts) => parts[0] === "ASKING"),
      true,
    );
    await editProfile("QA-faults", { follow: false });
    await script("GET qa:no-follow");
    await expect(lastCard()).toHaveClass(/response-error/);
    await expect(lastCard().locator("pre")).toContainText("ASK");
    fault.state.redirect = null;
    await editProfile("QA-faults", { follow: true });
  });
  await run("reload-command", "刷新页面后仍可取消正在执行的命令", async () => {
    await select("QA-faults");
    await clearResults();
    const before = fault.state.received.length;
    await script("GET qa:block");
    await expect
      .poll(() =>
        fault.state.received
          .slice(before)
          .some((parts) => parts[1] === "qa:block"),
      )
      .toBe(true);
    await page.reload();
    await expect(
      page.getByRole("button", { name: "取消", exact: true }),
    ).toBeVisible();
    await page.getByRole("button", { name: "取消", exact: true }).click();
    await expect(page.getByTestId("run-command")).toBeDisabled();
    await page.getByTestId("connect-current").click();
    await expect(page.locator(".connection-badge")).toHaveText("已连接");
  });
  await run("cancel-command", "取消等待中的命令，不执行后续写入", async () => {
    await clearResults();
    const before = fault.state.received.length;
    await script("GET qa:block\nSET qa:must-not-run yes");
    await expect
      .poll(() =>
        fault.state.received
          .slice(before)
          .some((parts) => parts[1] === "qa:block"),
      )
      .toBe(true);
    await page.getByRole("button", { name: "取消", exact: true }).click();
    await expect(page.getByTestId("run-command")).toBeDisabled();
    assert.equal(
      fault.state.received
        .slice(before)
        .some((parts) => parts[1] === "qa:must-not-run"),
      false,
    );
    await page.getByTestId("connect-current").click();
    await expect(page.locator(".connection-badge")).toHaveText("已连接");
  });
  await run(
    "command-timeout",
    "命令超时后显示失败并断开，重连可继续",
    async () => {
      await clearResults();
      await script("GET qa:block");
      await expect(lastCard().locator("pre")).toContainText("超时", {
        timeout: 8000,
      });
      await expect(page.getByTestId("run-command")).toBeDisabled();
      await page.getByTestId("connect-current").click();
      await expect(page.locator(".connection-badge")).toHaveText("已连接");
    },
  );
  await run(
    "disconnect-data",
    "断开连接后不继续展示旧数据为实时健康状态",
    async () => {
      await page.getByTestId("nav-monitoring").click();
      await expect(page.locator(".metric-value").nth(1)).toContainText("42");
      await page.getByRole("button", { name: "断开", exact: true }).click();
      await expect(page.locator(".metric-value").nth(1)).toHaveText("—keys");
      await select("QA-primary");
    },
  );
  await run(
    "password-removal",
    "显式移除密码后认证失败，并能修复",
    async () => {
      await editProfile("QA-primary", { forget: true }, false);
      await page.getByTestId("connect-current").click();
      await expect(page.locator(".connection-badge")).toHaveText("连接失败");
      await editProfile("QA-primary", { password });
    },
  );
  await run(
    "native-dialogs",
    "选择程序的成功与取消、导出写入失败",
    async () => {
      await page.getByTestId("open-local").click();
      await app.evaluate(({ dialog }, file) => {
        dialog.showOpenDialog = async () => ({
          canceled: false,
          filePaths: [file],
        });
      }, binary);
      await formItem("服务端程序").getByRole("button").click();
      await expect(
        page.getByTestId("local-binary").locator("input"),
      ).toHaveValue(binary);
      await app.evaluate(({ dialog }) => {
        dialog.showOpenDialog = async () => ({ canceled: true, filePaths: [] });
      });
      await formItem("服务端程序").getByRole("button").click();
      await expect(
        page.getByTestId("local-binary").locator("input"),
      ).toHaveValue(binary);
      await closeTools();
      await script("PING");
      await expect(lastCard().locator("pre")).toHaveText("PONG");
      await exportDialog(qa.directory);
      await page.getByTestId("nav-workbench").click();
      await page.getByRole("button", { name: "导出结果", exact: true }).click();
      await expect(page.locator(".toast-stack")).toContainText("EISDIR");
    },
  );
  await run("local-invalid", "本地启动无效程序及端口冲突", async () => {
    await page.getByTestId("open-local").click();
    await field("服务端程序", "/missing/miniredis");
    await page.getByTestId("start-local").click();
    await expect(page.locator(".toast-stack")).toContainText("程序不存在");
    await field("服务端程序", binary);
    await field("起始 RESP 端口", primary.port);
    await field("起始监控端口", primary.statsPort);
    await page.getByTestId("start-local").click();
    await expect(page.locator(".toast-stack")).toContainText("被占用");
    await closeTools();
  });

  for (const [mode, option, count] of [
    ["standalone", "单节点", 1],
    ["replication", "主从复制 · 2 节点", 2],
    ["cluster", "Cluster · 3 节点", 3],
    ["failover", "自动故障转移 · 3 节点", 3],
  ]) {
    await run(
      "local-" + mode,
      `实际界面启动、连接和停止 ${option}`,
      async () => {
        const range = await ports();
        await page.getByTestId("open-local").click();
        await choose("运行模式", option);
        await field("服务端程序", binary);
        await field("起始 RESP 端口", range.port);
        await field("起始监控端口", range.statsPort);
        await field("认证密码（可选）", "qa-local-password");
        await field("IO 线程", 2);
        await field("缓存分片", 4);
        await page.getByTestId("start-local").click();
        await expect(page.locator(".owned-node")).toHaveCount(count, {
          timeout: 20000,
        });
        await expect
          .poll(
            async () =>
              (await page.evaluate(() => window.desktop.localState())).filter(
                (node) => node.state === "running",
              ).length,
            { timeout: 20000 },
          )
          .toBe(count);
        await expect(page.locator(".connection-badge")).toHaveText("已连接");
        await closeTools();
        // An authenticated connection can precede the first quorum probe.
        if (mode === "failover") await waitWritable(range.port);
        await clearResults();
        await script('SET qa:local "persistent value"\nGET qa:local');
        await expect(page.getByTestId("result-card")).toHaveCount(2);
        await expect(
          page.getByTestId("result-card").first().locator("pre"),
        ).toHaveText("OK");
        await expect(lastCard().locator("pre")).toHaveText("persistent value");
        if (mode === "replication" || mode === "failover") {
          for (let index = 1; index < count; index++)
            await expect
              .poll(
                async () =>
                  (
                    await query(
                      range.port + index,
                      ["GET", "qa:local"],
                      "qa-local-password",
                    )
                  ).value,
                { timeout: 10000 },
              )
              .toBe("persistent value");
        }
        await page.getByTestId("nav-topology").click();
        await expect(page.locator(".topology-page")).toContainText("Primary");
        if (mode === "cluster")
          await expect(page.locator(".nodes-table tbody tr")).toHaveCount(3, {
            timeout: 10000,
          });
        if (mode === "cluster")
          await run(
            "cluster-operations",
            "集群槽位迁移、空目标校验和加入真实节点",
            async () => {
              let key;
              let slot;
              for (let i = 0; i < 30; i++) {
                key = `qa:move:${i}`;
                slot = Number(
                  (
                    await query(
                      range.port,
                      ["CLUSTER", "KEYSLOT", key],
                      "qa-local-password",
                    )
                  ).value,
                );
                if (slot < 5461) break;
              }
              assert.ok(slot < 5461);
              assert.equal(
                (
                  await query(
                    range.port,
                    ["SET", key, "migrated-value"],
                    "qa-local-password",
                  )
                ).type,
                "simple",
              );
              await page
                .getByRole("button", { name: "迁移槽位", exact: true })
                .click();
              await page
                .getByRole("button", { name: "执行并查看结果", exact: true })
                .click();
              await expect(page.locator(".toast-stack")).toContainText(
                "目标节点",
              );
              await field("槽位", slot);
              await field("目标节点", `127.0.0.1:${range.port + 1}`);
              await page
                .getByRole("button", { name: "执行并查看结果", exact: true })
                .click();
              await expect(lastCard()).not.toHaveClass(/response-error/);
              await expect
                .poll(
                  async () =>
                    (
                      await query(
                        range.port + 1,
                        ["GET", key],
                        "qa-local-password",
                      )
                    ).value,
                )
                .toBe("migrated-value");
              const peer = await redisFixture(join(qa.directory, "meet-peer"), {
                password: "qa-local-password",
                args: (address) => [
                  "--cluster",
                  "--node-addr",
                  `127.0.0.1:${address.port}`,
                  "--nodes",
                  `127.0.0.1:${address.port}`,
                ],
              });
              cleanup.push(peer.stop);
              await page.getByTestId("nav-topology").click();
              await page
                .getByRole("button", { name: "加入节点", exact: true })
                .click();
              await field("目标节点", `127.0.0.1:${peer.port}`);
              await page
                .getByRole("button", { name: "执行并查看结果", exact: true })
                .click();
              await expect(lastCard()).not.toHaveClass(/response-error/);
              await page.getByTestId("nav-topology").click();
              await expect(page.locator(".nodes-table tbody tr")).toHaveCount(
                4,
                { timeout: 10000 },
              );
            },
          );
        if (mode === "replication") {
          await page.getByTestId("open-local").click();
          await page
            .getByRole("button", { name: "连接此节点", exact: true })
            .nth(1)
            .click();
          await closeTools();
          await page.getByTestId("nav-topology").click();
          await expect(page.locator(".node-identity .role-tag")).toHaveText(
            "Replica",
            { timeout: 10000 },
          );
          await expect(
            page.getByRole("button", { name: "手动接管主节点", exact: true }),
          ).toHaveCount(0);
          await script("SET qa:readonly rejected");
          await expect(lastCard()).toHaveClass(/response-error/);
        }
        if (mode === "failover") {
          await page.getByTestId("open-local").click();
          await page
            .getByRole("button", { name: "停止此节点", exact: true })
            .first()
            .click();
          await page.getByRole("button", { name: "确认", exact: true }).click();
          await page
            .getByRole("button", { name: "连接此节点", exact: true })
            .nth(1)
            .click();
          await closeTools();
          await expect
            .poll(
              async () =>
                (
                  await query(
                    range.port + 1,
                    ["INFO", "replication"],
                    "qa-local-password",
                  )
                ).value.includes("role:master"),
              { timeout: 20000 },
            )
            .toBe(true);
          await page.getByTestId("nav-topology").click();
          await expect(page.locator(".node-identity .role-tag")).toHaveText(
            "Primary",
            { timeout: 12000 },
          );
          await waitWritable(range.port + 1);
          await clearResults();
          await script(
            'GET qa:local\nSET qa:after-failover "elected value"\nGET qa:after-failover',
          );
          await expect(page.getByTestId("result-card")).toHaveCount(3);
          await expect(
            page.getByTestId("result-card").nth(0).locator("pre"),
          ).toHaveText("persistent value");
          await expect(
            page.getByTestId("result-card").nth(1).locator("pre"),
          ).toHaveText("OK");
          await expect(lastCard().locator("pre")).toHaveText("elected value");
          await expect
            .poll(
              async () =>
                (
                  await query(
                    range.port + 2,
                    ["GET", "qa:after-failover"],
                    "qa-local-password",
                  )
                ).value,
              { timeout: 10000 },
            )
            .toBe("elected value");
          await page.getByTestId("nav-topology").click();
        }
        await snapshot("local-" + mode);
        await page.getByTestId("open-local").click();
        await page
          .getByRole("button", { name: "停止全部", exact: true })
          .click();
        await expect
          .poll(
            async () =>
              (await page.evaluate(() => window.desktop.localState())).every(
                (node) => node.state === "stopped",
              ),
            { timeout: 10000 },
          )
          .toBe(true);
        if (mode === "standalone") {
          await page.getByTestId("start-local").click();
          await expect(page.locator(".connection-badge")).toHaveText("已连接", {
            timeout: 10000,
          });
          await closeTools();
          await clearResults();
          await script("GET qa:local");
          await expect(lastCard().locator("pre")).toHaveText(
            "persistent value",
          );
          await page.getByTestId("open-local").click();
          await page
            .getByRole("button", { name: "停止全部", exact: true })
            .click();
          await expect
            .poll(async () =>
              (await page.evaluate(() => window.desktop.localState())).every(
                (node) => node.state === "stopped",
              ),
            )
            .toBe(true);
        }
        await closeTools();
      },
    );
    // Roll back only processes owned by this isolated application if a mode failed.
    await page.evaluate(() => window.desktop.stopLocal());
    await closeTools();
  }
  await select("QA-primary");
  await run("manual-takeover", "集群副本手动接管的取消和确认", async () => {
    const masterAddress = await ports();
    const replicaAddress = await ports();
    const masterNode = `127.0.0.1:${masterAddress.port}`;
    const replicaNode = `127.0.0.1:${replicaAddress.port}`;
    const nodes = `${masterNode},${replicaNode}`;
    const master = await redisFixture(join(qa.directory, "takeover-master"), {
      addresses: masterAddress,
      password: "qa-takeover",
      args: [
        "--cluster",
        "--node-addr",
        masterNode,
        "--nodes",
        nodes,
        "--replicas",
        replicaNode,
      ],
    });
    cleanup.push(master.stop);
    const replica = await redisFixture(join(qa.directory, "takeover-replica"), {
      addresses: replicaAddress,
      password: "qa-takeover",
      args: [
        "--cluster",
        "--node-addr",
        replicaNode,
        "--nodes",
        nodes,
        "--replicaof",
        masterNode,
      ],
    });
    cleanup.push(replica.stop);
    await addProfile({
      name: "QA-takeover",
      port: replica.port,
      stats: replica.statsPort,
      password: "qa-takeover",
    });
    await page.getByTestId("nav-topology").click();
    await expect(page.locator(".node-identity .role-tag")).toHaveText(
      "Replica",
    );
    await page
      .getByRole("button", { name: "手动接管主节点", exact: true })
      .click();
    await page.getByRole("button", { name: "取消", exact: true }).click();
    await page
      .getByRole("button", { name: "手动接管主节点", exact: true })
      .click();
    await page.getByRole("button", { name: "确认", exact: true }).click();
    await expect(lastCard().locator("pre")).toHaveText("OK");
    await page.getByTestId("nav-topology").click();
    await expect(page.locator(".node-identity .role-tag")).toHaveText(
      "Primary",
    );
    await expect(
      page.getByRole("button", { name: "手动接管主节点", exact: true }),
    ).toHaveCount(0);
    await select("QA-primary");
  });
  const taskCases = [
    ["benchmark", "性能压测"],
    ["smoke", "协议与接口验证"],
    ["recovery", "恢复与稳定性"],
    ["resources", "资源与故障验证"],
    ["replication", "复制与断线追平"],
    ["cluster", "集群路由验证"],
    ["failover", "自动故障转移"],
  ];
  for (const [kind, label] of taskCases) {
    await run("task-" + kind, `任务中心真实运行：${label}`, async () => {
      await page.getByRole("button", { name: "任务中心", exact: true }).click();
      await page.getByTestId("task-binary").locator("input").fill(binary);
      await choose("任务类型", label);
      for (const [name, value] of [
        ["requests", 1000],
        ["clients", 2],
        ["payload", 64],
        ["runs", 1],
      ]) {
        const input = page.getByTestId("task-" + name).locator("input");
        if (await input.count()) {
          await input.fill(String(value));
          await input.blur();
        }
      }
      await page.getByRole("button", { name: "运行任务", exact: true }).click();
      await expect
        .poll(
          async () =>
            (await page.evaluate(() => window.desktop.taskState()))?.status,
          { timeout: 60000 },
        )
        .not.toBe("running");
      const task = await page.evaluate(() => window.desktop.taskState());
      assert.equal(task?.status, "completed", JSON.stringify(task));
      await expect(page.locator(".task-status-card")).toContainText("已完成");
      await closeTools();
    });
    await page.evaluate(() => window.desktop.stopTask());
    await closeTools();
  }
  await run("task-cancel", "运行中取消任务并恢复可运行状态", async () => {
    await page.getByRole("button", { name: "任务中心", exact: true }).click();
    await choose("任务类型", "性能压测");
    await page.getByTestId("task-requests").locator("input").fill("1000000");
    await page.getByTestId("task-requests").locator("input").blur();
    await page.getByRole("button", { name: "运行任务", exact: true }).click();
    await expect(page.locator(".task-status-card")).toContainText("运行中");
    await page.getByRole("button", { name: "取消任务", exact: true }).click();
    await expect(page.locator(".task-status-card")).toContainText("已取消", {
      timeout: 10000,
    });
    await expect(
      page.getByRole("button", { name: "运行任务", exact: true }),
    ).toBeEnabled();
    await closeTools();
  });
  await run("task-failure", "任务程序错误与失败状态", async () => {
    const failing = join(qa.directory, "failing-server");
    await writeFile(failing, "#!/bin/sh\nexit 7\n", { mode: 0o700 });
    await page.getByRole("button", { name: "任务中心", exact: true }).click();
    await page.getByTestId("task-binary").locator("input").fill(failing);
    await page.getByRole("button", { name: "运行任务", exact: true }).click();
    await expect(page.locator(".task-status-card")).toContainText("执行失败", {
      timeout: 15000,
    });
    await closeTools();
  });
  await run("log-controls", "日志展开、导出、清空和关闭", async () => {
    if (!(await page.locator(".log-dock").isVisible()))
      await page.getByRole("button", { name: "日志", exact: true }).click();
    const output = join(qa.directory, "logs.txt");
    await exportDialog(output);
    await page.getByRole("button", { name: "导出日志", exact: true }).click();
    await expect
      .poll(async () =>
        (await readFile(output, "utf8").catch(() => "")).includes("任务开始"),
      )
      .toBe(true);
    await page.getByRole("button", { name: "清空视图", exact: true }).click();
    await expect(page.locator(".log-dock .log-line")).toHaveCount(0);
    await page.getByRole("button", { name: "关闭日志", exact: true }).click();
    await expect(page.locator(".log-dock")).not.toBeVisible();
  });
  await run(
    "compact-layout",
    "最小窗口中命令参考、执行按钮和结果区可用",
    async () => {
      await select("QA-primary");
      await page.getByTestId("nav-workbench").click();
      await app.evaluate(({ BrowserWindow }) =>
        BrowserWindow.getAllWindows()[0].setSize(1060, 700),
      );
      await page.getByRole("button", { name: "命令参考", exact: true }).click();
      const button = await page.getByTestId("run-command").boundingBox();
      assert.ok(
        button &&
          button.x >= 0 &&
          button.x + button.width <= (await page.evaluate(() => innerWidth)),
        "run control is clipped at minimum width",
      );
      await script("PING");
      await expect(lastCard().locator("pre")).toHaveText("PONG");
      await snapshot("compact-window");
      await page.getByRole("button", { name: "命令参考", exact: true }).click();
      await app.evaluate(({ BrowserWindow }) =>
        BrowserWindow.getAllWindows()[0].setSize(1440, 940),
      );
    },
  );
  await run(
    "theme-persistence",
    "浅色、深色、跟随系统与重载后的设置恢复",
    async () => {
      await page.getByRole("button", { name: "设置", exact: true }).click();
      for (const [label, theme] of [
        ["深色", "dark"],
        ["浅色", "light"],
        ["跟随系统", "system"],
      ]) {
        await page.getByRole("button", { name: label, exact: true }).click();
        assert.equal(
          (await page.evaluate(() => window.desktop.workspace())).theme,
          theme,
        );
      }
      await page.getByRole("button", { name: "深色", exact: true }).click();
      await closeTools();
      await page.reload();
      await expect(page.locator("html")).toHaveAttribute("data-theme", "dark");
      await snapshot("dark-reloaded");
    },
  );
  await run(
    "delete-profile",
    "删除取消、确认删除与最后一个连接的空状态",
    async () => {
      const profiles = (await page.evaluate(() => window.desktop.workspace()))
        .profiles;
      for (const profile of profiles) {
        await page
          .getByRole("button", {
            name: "管理连接 " + profile.name,
            exact: true,
          })
          .click();
        await page.getByText("删除连接", { exact: true }).click();
        if (profile === profiles[0]) {
          await page.getByRole("button", { name: "取消", exact: true }).click();
          await page
            .getByRole("button", {
              name: "管理连接 " + profile.name,
              exact: true,
            })
            .click();
          await page.getByText("删除连接", { exact: true }).click();
        }
        await page
          .getByRole("dialog")
          .getByRole("button", { name: "删除连接", exact: true })
          .click();
        await expect(
          page.getByRole("button", {
            name: "选择连接 " + profile.name,
            exact: true,
          }),
        ).toHaveCount(0);
      }
      await expect(page.locator(".workspace-content")).toContainText("连接");
      await snapshot("empty-workspace");
    },
  );
  await run(
    "quit-and-restart",
    "运行中退出回收实例/任务，重启恢复设置和安全密码策略",
    async () => {
      await page.evaluate(
        async ({
          binary,
          port,
          statsPort,
          primaryPort,
          primaryStats,
          password,
        }) => {
          const profile = await window.desktop.saveConnection({
            name: "QA-restart",
            host: "127.0.0.1",
            port: primaryPort,
            statsPort: primaryStats,
            username: "",
            password,
            color: "#123456",
            followRedirects: true,
          });
          await window.desktop.preferences({
            theme: "dark",
            selectedId: profile.id,
          });
          await window.desktop.startLocal({
            binary,
            mode: "standalone",
            port,
            statsPort,
            password: "",
            aof: false,
            ioThreads: 2,
            shards: 4,
            memoryMiB: 16,
            eviction: "noeviction",
          });
          await window.desktop.runTask({
            binary,
            kind: "benchmark",
            requests: 1000000,
            clients: 1,
            payload: 64,
            runs: 5,
          });
        },
        {
          binary,
          ...(await ports()),
          primaryPort: primary.port,
          primaryStats: primary.statsPort,
          password,
        },
      );
      const owned = await page.evaluate(() => window.desktop.localState());
      await delay(200);
      const processIdentity = async (pid) => {
        try {
          const text = await readFile(`/proc/${pid}/stat`, "utf8");
          const fields = text.slice(text.lastIndexOf(")") + 2).split(" ");
          return { state: fields[0], start: fields[19] };
        } catch {
          return null;
        }
      };
      const descendants = new Set([app.process().pid]);
      for (const pid of descendants) {
        const children = await readFile(
          `/proc/${pid}/task/${pid}/children`,
          "utf8",
        ).catch(() => "");
        for (const child of children.trim().split(/\s+/).filter(Boolean))
          descendants.add(Number(child));
      }
      const identities = new Map(
        await Promise.all(
          [...descendants].map(async (pid) => [
            pid,
            await processIdentity(pid),
          ]),
        ),
      );
      await qa.shutdown();
      await expect
        .poll(
          async () => {
            const alive = [];
            for (const [pid, identity] of identities) {
              const now = await processIdentity(pid);
              if (now && now.state !== "Z" && now.start === identity?.start)
                alive.push(pid);
            }
            return alive;
          },
          { timeout: 5000 },
        )
        .toEqual([]);
      for (const node of owned) {
        let alive = false;
        try {
          process.kill(node.pid, 0);
          alive = true;
        } catch {}
        assert.equal(alive, false, `owned PID ${node.pid} survived exit`);
      }
      const restarted = await harness({
        directory: qa.directory,
        keepData: true,
      });
      try {
        await expect(restarted.page.locator("html")).toHaveAttribute(
          "data-theme",
          "dark",
        );
        const workspace = await restarted.page.evaluate(() =>
          window.desktop.workspace(),
        );
        assert.equal(workspace.profiles[0].name, "QA-restart");
        assert.deepEqual(workspace.running, []);
        assert.deepEqual(workspace.connections, []);
        if (workspace.passwordStorage === "session") {
          assert.equal(workspace.profiles[0].hasPassword, false);
          await restarted.page.getByTestId("connect-current").click();
          await expect(restarted.page.locator(".connection-badge")).toHaveText(
            "连接失败",
          );
          await restarted.page
            .getByRole("button", { name: "管理连接 QA-restart", exact: true })
            .click();
          await restarted.page.getByText("编辑连接", { exact: true }).click();
          await restarted.page
            .getByTestId("connection-password")
            .locator("input")
            .fill(password);
          await restarted.page.getByTestId("connection-save").click();
        } else await restarted.page.getByTestId("connect-current").click();
        await expect(restarted.page.locator(".connection-badge")).toHaveText(
          "已连接",
        );
        await expect(restarted.page.getByTestId("run-command")).toBeEnabled();
        assert.deepEqual(restarted.errors, []);
      } finally {
        await restarted.close();
      }
    },
  );
  if (process.env.MINIREDIS_TEST_ELECTRON)
    await run(
      "standalone-package",
      "独立安装包缺少源码时禁用脚本并保留远程连接入口",
      async () => {
        const standalone = await harness({ withoutSource: true });
        try {
          const workspace = await standalone.page.evaluate(() =>
            window.desktop.workspace(),
          );
          assert.equal(workspace.projectRoot, null);
          await standalone.page
            .getByRole("button", { name: "任务中心", exact: true })
            .click();
          await expect(
            standalone.page.getByRole("button", {
              name: "运行任务",
              exact: true,
            }),
          ).toBeDisabled();
          await expect(standalone.page.locator(".n-drawer")).toContainText(
            "源码目录",
          );
          await standalone.page.locator(".n-drawer-header__close").click();
          await expect(
            standalone.page.getByTestId("add-connection"),
          ).toBeEnabled();
          await standalone.page.screenshot({
            path: join(qa.artifacts, "standalone-no-source.png"),
          });
          assert.deepEqual(standalone.errors, []);
        } finally {
          await standalone.close();
        }
      },
    );
  await run("renderer-errors", "无未处理界面异常或控制台错误", async () => {
    assert.deepEqual(qa.errors, []);
  });
} finally {
  await qa.report();
  await qa.shutdown();
  for (const close of cleanup.reverse()) await close();
  await qa.close();
}
const failed = qa.cases.filter((item) => item.status === "failed");
console.log(
  `Desktop branches: ${qa.cases.length - failed.length}/${qa.cases.length} passed. Report: ${qa.artifacts}`,
);
if (failed.length) process.exitCode = 1;
