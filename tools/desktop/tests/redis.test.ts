import net from "node:net";
import { afterEach, describe, expect, it } from "vitest";
import { RedisSession, RespConnection } from "../src/main/redis";
import { RespDecoder } from "../src/main/protocol";
import type {
  ConnectionProfile,
  DesktopEvent,
  RespValue,
} from "../src/shared/contracts";

const cleanup: (() => Promise<void> | void)[] = [];
afterEach(async () => {
  for (const close of cleanup.splice(0).reverse()) await close();
});
async function server(
  reply: (parts: string[]) => string | undefined,
): Promise<number> {
  const sockets = new Set<net.Socket>();
  const server = net.createServer((socket) => {
    sockets.add(socket);
    // Cancellation deliberately closes peers while responses may still be in flight.
    socket.on("error", () => {});
    socket.on("close", () => sockets.delete(socket));
    const decoder = new RespDecoder();
    socket.on("data", (chunk) => {
      for (const frame of decoder.feed(chunk)) {
        if (frame.type !== "array") throw new Error("expected array");
        const parts = frame.value.map((value) =>
          "value" in value ? String(value.value) : "",
        );
        const answer =
          reply(parts) ??
          (parts[0] === "PING"
            ? "+PONG\r\n"
            : parts[0] === "INFO"
              ? "$13\r\nrole:master\r\n\r\n"
              : undefined);
        if (answer) socket.write(answer);
      }
    });
  });
  await new Promise<void>((done) => server.listen(0, "127.0.0.1", done));
  cleanup.push(
    () =>
      new Promise<void>((done) => {
        for (const socket of sockets) socket.destroy();
        server.close(() => done());
      }),
  );
  return (server.address() as net.AddressInfo).port;
}
function profile(port: number): ConnectionProfile {
  return {
    id: "test",
    name: "Test",
    host: "127.0.0.1",
    port,
    statsPort: 8080,
    username: "",
    color: "#123456",
    followRedirects: true,
    hasPassword: false,
  };
}
async function session(
  port: number,
  events: DesktopEvent[],
  password = "",
): Promise<RedisSession> {
  const session = new RedisSession(profile(port), "", password, (event) =>
    events.push(event),
  );
  cleanup.push(() => session.disconnect());
  await session.connect();
  return session;
}

describe("live RESP sessions", () => {
  it("sends named authentication even when the named account has an empty password", async () => {
    const received: string[][] = [];
    const port = await server((parts) => {
      received.push(parts);
      return parts[0] === "AUTH" ? "+OK\r\n" : undefined;
    });
    const connection = new RespConnection("127.0.0.1", port);
    cleanup.push(() => connection.close());
    await connection.open("nopass-user", "");
    expect(received[0]).toEqual(["AUTH", "nopass-user", ""]);
  });
  it("respects redirect settings and bounds cycles", async () => {
    let address = "";
    const port = await server((parts) =>
      parts[0] === "GET" ? `-MOVED 1 ${address}\r\n` : undefined,
    );
    address = `127.0.0.1:${port}`;
    const events: DesktopEvent[] = [];
    const disabled = new RedisSession(
      { ...profile(port), followRedirects: false },
      "",
      "",
      (event) => events.push(event),
    );
    cleanup.push(() => disabled.disconnect());
    await disabled.connect();
    await disabled.execute("GET a", "disabled");
    expect(
      events.filter((event) => event.type === "result").at(-1)?.result.response,
    ).toEqual({ type: "error", value: `MOVED 1 ${address}` });
    const followed = await session(port, events);
    await followed.execute("GET a", "cycle");
    expect(
      events.filter((event) => event.type === "result").at(-1)?.result.response,
    ).toMatchObject({ type: "error", value: expect.stringContaining("3 次") });
  });
  it("rejects malformed redirects and denied ASKING without sending the write", async () => {
    const denied: string[][] = [];
    const target = await server((parts) => {
      denied.push(parts);
      return parts[0] === "ASKING" ? "-NOPERM denied\r\n" : undefined;
    });
    const origin = await server((parts) =>
      parts[0] === "SET"
        ? `-ASK 1 127.0.0.1:${target}\r\n`
        : parts[0] === "GET"
          ? "-MOVED 1 missing-port\r\n"
          : undefined,
    );
    const events: DesktopEvent[] = [];
    const connection = await session(origin, events);
    await connection.execute("SET a b\nGET a", "bad");
    expect(denied.some((parts) => parts[0] === "SET")).toBe(false);
    const results = events.filter((event) => event.type === "result");
    expect(results[0].result.response).toMatchObject({
      type: "error",
      value: "NOPERM denied",
    });
    expect(results[1].result.transportError).toBe(true);
  });
  it("cleans up a cancelled connection attempt and recovers after a malformed protocol frame", async () => {
    let notify!: () => void;
    const pending = new Promise<void>((done) => {
      notify = done;
    });
    const port = await server((parts) => {
      if (parts[0] === "INFO") notify();
      return parts[0] === "INFO"
        ? "$13\r\nrole:master\r\n\r\n"
        : parts[0] === "BAD"
          ? "?invalid\r\n"
          : undefined;
    });
    const events: DesktopEvent[] = [];
    const connection = new RedisSession(profile(port), "", "", (event) =>
      events.push(event),
    );
    cleanup.push(() => connection.disconnect());
    const connecting = connection.connect();
    await pending;
    connection.disconnect();
    await expect(connecting).rejects.toThrow();
    expect(connection.state.status).toBe("disconnected");
    await connection.connect();
    await connection.execute("BAD\nSET must-not-run yes", "malformed");
    expect(connection.state.status).toBe("error");
    expect(events.filter((event) => event.type === "result")).toHaveLength(1);
  });
  it("rejects offline commands and propagates INFO permission failures", async () => {
    let deny = false;
    const port = await server((parts) =>
      deny && parts[0] === "INFO" ? "-NOPERM INFO denied\r\n" : undefined,
    );
    const events: DesktopEvent[] = [];
    const connection = await session(port, events);
    deny = true;
    await expect(connection.topology()).rejects.toThrow("denied");
    connection.disconnect();
    await connection.execute("PING", "offline");
    expect(
      events.filter((event) => event.type === "result").at(-1)?.result
        .transportError,
    ).toBe(true);
  });
  it("retries the exact redirected command and retains repeated responses", async () => {
    const received: string[][] = [];
    const target = await server((parts) => {
      received.push(parts);
      return parts[0] === "GET" ? "$4\r\nsame\r\n" : undefined;
    });
    const origin = await server((parts) =>
      parts[0] === "GET" ? `-MOVED 42 127.0.0.1:${target}\r\n` : undefined,
    );
    const events: DesktopEvent[] = [];
    const connection = await session(origin, events);
    expect(
      await connection.execute("GET first\nGET second\nGET second", "run"),
    ).toEqual({ count: 3, cancelled: false });
    expect(received.filter((parts) => parts[0] === "GET")).toEqual([
      ["GET", "first"],
      ["GET", "second"],
      ["GET", "second"],
    ]);
    const results = events
      .filter((event) => event.type === "result")
      .map((event) => event.result);
    expect(results).toHaveLength(3);
    expect(results.map((result) => result.command)).toEqual([
      "GET first",
      "GET second",
      "GET second",
    ]);
    expect(
      results.every(
        (result) =>
          result.response.type === "bulk" && result.response.value === "same",
      ),
    ).toBe(true);
  });
  it("sends ASKING immediately before the redirected command", async () => {
    const received: string[][] = [];
    const target = await server((parts) => {
      received.push(parts);
      return ["ASKING", "SET"].includes(parts[0]) ? "+OK\r\n" : undefined;
    });
    const origin = await server((parts) =>
      parts[0] === "SET" ? `-ASK 4 127.0.0.1:${target}\r\n` : undefined,
    );
    const connection = await session(origin, []);
    await connection.execute("SET key value", "ask");
    expect(received.slice(-2)).toEqual([["ASKING"], ["SET", "key", "value"]]);
  });
  it("does not report an authenticated connection on AUTH failure", async () => {
    const port = await server((parts) =>
      parts[0] === "AUTH" ? "-ERR WRONGPASS\r\n" : undefined,
    );
    const events: DesktopEvent[] = [];
    await expect(session(port, events, "private-fixture")).rejects.toThrow(
      "认证失败",
    );
    expect(
      events.some(
        (event) =>
          event.type === "connection" && event.state.status === "connected",
      ),
    ).toBe(false);
    expect(JSON.stringify(events)).not.toContain("private-fixture");
  });
  it("cancels pending work without sending later commands", async () => {
    let notify!: () => void;
    const pending = new Promise<void>((done) => {
      notify = done;
    });
    const received: string[][] = [];
    const port = await server((parts) => {
      received.push(parts);
      if (parts[0] === "BLOCK") notify();
      return undefined;
    });
    const connection = await session(port, []);
    const work = connection.execute("BLOCK\nSET must-not-run yes", "cancel");
    await pending;
    connection.cancel();
    expect(await work).toEqual({ count: 0, cancelled: true });
    expect(received.some((parts) => parts[0] === "SET")).toBe(false);
  });
  it("closes timed-out pipelines to prevent assigning a late reply to another command", async () => {
    const port = await server(() => undefined);
    const connection = new RespConnection("127.0.0.1", port, 40);
    cleanup.push(() => connection.close());
    await connection.open();
    const requests = Promise.allSettled([
      connection.request(["BLOCK"]),
      connection.request(["OTHER"]),
    ]);
    expect(
      (await requests).every((result) => result.status === "rejected"),
    ).toBe(true);
    await expect(connection.request(["PING"])).rejects.toThrow("断开");
  });
});
