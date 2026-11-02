import assert from "node:assert/strict";
import net from "node:net";
import { spawn } from "node:child_process";
import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
import { setTimeout as delay } from "node:timers/promises";
import { _electron, expect } from "playwright/test";
import { RespDecoder, encodeCommand } from "../src/main/protocol.ts";

export { assert, expect, delay };
export const desktop = resolve(dirname(fileURLToPath(import.meta.url)), "..");
export const root = resolve(desktop, "../..");
export const binary = resolve(
  process.env.MINIREDIS_TEST_BINARY || join(root, "build/miniredis"),
);

export async function ports(count = 3) {
  for (let attempt = 0; attempt < 50; attempt++) {
    const port = 22000 + Math.floor(Math.random() * 30000);
    const listeners = [];
    try {
      for (const candidate of Array.from({ length: count }, (_, i) => [
        port + i,
        port + 10 + i,
      ]).flat()) {
        const listener = net.createServer();
        listeners.push(listener);
        await new Promise((done, reject) => {
          listener.once("error", reject);
          listener.listen(candidate, "127.0.0.1", done);
        });
      }
      return { port, statsPort: port + 10 };
    } catch {
      /* Try a different free range. */
    } finally {
      await Promise.all(
        listeners.map(
          (listener) => new Promise((done) => listener.close(() => done())),
        ),
      );
    }
  }
  throw new Error("Cannot allocate test ports");
}

export async function query(port, parts, password = "", username = "") {
  return new Promise((resolveQuery, reject) => {
    const socket = net.connect(port, "127.0.0.1");
    const decoder = new RespDecoder();
    const commands = password
      ? [username ? ["AUTH", username, password] : ["AUTH", password], parts]
      : [parts];
    let received = 0;
    socket.setTimeout(5000, () =>
      socket.destroy(new Error("Fixture query timed out")),
    );
    socket.once("error", reject);
    socket.once("connect", () =>
      socket.write(Buffer.concat(commands.map(encodeCommand))),
    );
    socket.on("data", (chunk) => {
      try {
        for (const frame of decoder.feed(chunk))
          if (++received === commands.length) {
            socket.destroy();
            resolveQuery(frame);
          }
      } catch (error) {
        socket.destroy();
        reject(error);
      }
    });
  });
}

export async function redisFixture(directory, options = {}) {
  const addresses = options.addresses || (await ports());
  await mkdir(directory, { recursive: true });
  const env = Object.fromEntries(
    Object.entries(process.env).filter(
      ([key]) => !key.startsWith("MINIREDIS_"),
    ),
  );
  if (options.password) env.MINIREDIS_REQUIREPASS = options.password;
  const args = [
    "--bind",
    "127.0.0.1",
    "--port",
    String(addresses.port),
    "--stats-bind",
    "127.0.0.1",
    "--stats-port",
    String(addresses.statsPort),
    "--snapshot-file",
    join(directory, "snapshot.dat"),
    "--io-threads",
    "2",
    "--cache-shards",
    "4",
    ...(typeof options.args === "function"
      ? options.args(addresses)
      : options.args || []),
  ];
  const child = spawn(binary, args, { env, stdio: ["ignore", "pipe", "pipe"] });
  let output = "";
  child.stdout.on("data", (data) => {
    output = (output + data).slice(-16_384);
  });
  child.stderr.on("data", (data) => {
    output = (output + data).slice(-16_384);
  });
  child.on("error", (error) => {
    output += error.message;
  });
  for (let attempt = 0; attempt < 100; attempt++) {
    try {
      if (
        (await fetch(`http://127.0.0.1:${addresses.statsPort}/healthz`))
          .status === 200
      )
        return {
          ...addresses,
          child,
          stop: async () => {
            child.kill("SIGTERM");
            await new Promise((done) =>
              child.exitCode !== null || child.signalCode !== null
                ? done()
                : child.once("exit", done),
            );
          },
        };
    } catch {
      /* Wait until this test's server is ready. */
    }
    if (child.exitCode !== null)
      throw new Error(`Fixture server failed: ${output}`);
    await delay(50);
  }
  child.kill("SIGKILL");
  throw new Error(`Fixture server did not become ready: ${output}`);
}

export async function harness(options = {}) {
  const require = createRequire(import.meta.url);
  const directory =
    options.directory ||
    (await mkdtemp(join(tmpdir(), "miniredis-branch-qa-")));
  const artifacts = resolve(
    process.env.MINIREDIS_QA_OUTPUT || join(root, "build-review/desktop-qa"),
  );
  await mkdir(artifacts, { recursive: true });
  const env = Object.fromEntries(
    Object.entries(process.env).filter(
      ([key]) => key !== "ELECTRON_RUN_AS_NODE",
    ),
  );
  const args = [desktop];
  if (process.env.MINIREDIS_TEST_NO_SANDBOX === "1")
    args.unshift("--no-sandbox");
  if (options.withoutSource) delete env.MINIREDIS_PROJECT_ROOT;
  const app = await _electron.launch({
    executablePath: process.env.MINIREDIS_TEST_ELECTRON || require("electron"),
    args,
    cwd: options.withoutSource ? directory : root,
    env: {
      ...env,
      MINIREDIS_DESKTOP_DATA: directory,
      ...(options.withoutSource ? {} : { MINIREDIS_PROJECT_ROOT: root }),
    },
    timeout: 30_000,
  });
  const page = await app.firstWindow();
  page.setDefaultTimeout(6000);
  const errors = [];
  const cases = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (message) => {
    if (message.type() === "error") errors.push(message.text());
  });
  await expect(page.getByTestId("connect-current")).toBeVisible();
  async function run(key, title, operation) {
    const selected = process.env.MINIREDIS_QA_CASES?.split(",");
    if (selected && !selected.includes(key)) return;
    const started = Date.now();
    try {
      await operation();
      cases.push({
        key,
        title,
        status: "passed",
        durationMs: Date.now() - started,
      });
      console.log(`PASS ${key}: ${title}`);
    } catch (error) {
      cases.push({
        key,
        title,
        status: "failed",
        durationMs: Date.now() - started,
        error: error.message,
      });
      await page
        .screenshot({ path: join(artifacts, `${key}-failed.png`) })
        .catch(() => {});
      console.error(`FAIL ${key}: ${error.message.slice(0, 1400)}`);
      await page.keyboard.press("Escape").catch(() => {});
    }
  }
  async function report() {
    const value = {
      platform: process.platform,
      executable:
        process.env.MINIREDIS_TEST_ELECTRON ||
        "development Electron with production bundle",
      cases,
      rendererErrors: errors,
    };
    await writeFile(
      join(artifacts, "report.json"),
      JSON.stringify(value, null, 2),
    );
    await writeFile(
      join(artifacts, "report.md"),
      `# Desktop workflow verification\n\n${cases.filter((item) => item.status === "passed").length}/${cases.length} passed.\n\n| Case | Workflow | Result |\n| --- | --- | --- |\n` +
        cases
          .map(
            (item) =>
              `| ${item.key} | ${item.title} | ${item.status}${item.error ? ": " + item.error.split("\n")[0].replaceAll("|", "/") : ""} |`,
          )
          .join("\n") +
        `\n\nRenderer errors: ${JSON.stringify(errors)}\n`,
    );
  }
  let shutdownPromise;
  const shutdown = () => (shutdownPromise ??= app.close());
  return {
    shutdown,
    app,
    page,
    directory,
    artifacts,
    errors,
    cases,
    run,
    report,
    close: async () => {
      await shutdown();
      if (!options.keepData)
        await rm(directory, { recursive: true, force: true });
    },
  };
}
