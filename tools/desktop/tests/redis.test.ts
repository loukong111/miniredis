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
