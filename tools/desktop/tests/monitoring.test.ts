import http from "node:http";
import type { AddressInfo } from "node:net";
import { afterEach, describe, expect, it } from "vitest";
import {
  fetchEndpoint,
  monitoring,
  parseAddress,
  parseInfo,
  asError,
} from "../src/main/redis";
import type { ConnectionProfile } from "../src/shared/contracts";

const closers: (() => Promise<void>)[] = [];
afterEach(async () => {
  for (const close of closers.splice(0)) await close();
});
async function endpoint(stats: {
  body?: string;
  status?: number;
  health?: number;
  ready?: number;
  hang?: boolean;
}) {
  const server = http.createServer((request, response) => {
    if (stats.hang) return;
    if (request.url === "/healthz" || request.url === "/readyz") {
      response.writeHead(
        request.url === "/healthz"
          ? (stats.health ?? 200)
          : (stats.ready ?? 200),
      );
      response.end("status");
      return;
    }
    response.writeHead(stats.status ?? 200);
    response.end(stats.body ?? '{"total_commands":1,"key_count":2}');
  });
  await new Promise<void>((done) => server.listen(0, "127.0.0.1", done));
  closers.push(
    () =>
      new Promise<void>((done) => {
        server.closeAllConnections();
        server.close(() => done());
      }),
  );
  const profile: ConnectionProfile = {
    id: "http",
    name: "HTTP fixture",
    host: "127.0.0.1",
    port: 6366,
    statsPort: (server.address() as AddressInfo).port,
    username: "",
    hasPassword: false,
    color: "#123456",
    followRedirects: true,
  };
  return {
    profile,
    close: async () => {
      server.closeAllConnections();
      await new Promise<void>((done) => server.close(() => done()));
    },
  };
}

describe("monitoring transport", () => {
  it("keeps health and readiness independent from command metrics", async () => {
    const { profile } = await endpoint({ health: 500, ready: 503 });
    expect(await monitoring(profile)).toMatchObject({
      healthy: false,
      ready: false,
      stats: { key_count: 2 },
      connectionId: "http",
    });
  });
  it.each(["null", "[]", '"text"', "broken-json"])(
    "rejects invalid stats documents",
    async (body) => {
      const { profile } = await endpoint({ body });
      const value = await monitoring(profile);
      expect(value.error).toContain("无法读取");
      expect(value.stats).toBeUndefined();
    },
  );
  it("ignores nested/null fields without inventing measurements", async () => {
    const { profile } = await endpoint({
      body: '{"key_count":2,"nested":{"a":1},"nil":null,"enabled":true,"node":"a"}',
    });
    expect((await monitoring(profile)).stats).toEqual({
      key_count: 2,
      enabled: true,
      node: "a",
    });
  });
  it("reports status failures, unavailable HTTP and excessive response size", async () => {
    const bad = await endpoint({ status: 503 });
    expect((await monitoring(bad.profile)).error).toContain("503");
    await bad.close();
    expect((await monitoring(bad.profile)).healthy).toBeUndefined();
    expect((await monitoring(bad.profile)).error).toContain("无法读取");
    const large = await endpoint({ body: "x".repeat(1024 * 1024 + 1) });
    await expect(fetchEndpoint(large.profile, "/stats")).rejects.toThrow(
      "1 MiB",
    );
  });
  it("aborts an endpoint that never replies", async () => {
    const { profile } = await endpoint({ hang: true });
    await expect(fetchEndpoint(profile, "/stats")).rejects.toThrow();
  });
});

describe("server metadata", () => {
  it("parses IPv6 addresses and INFO fields with embedded colons", () => {
    expect(parseAddress("[::1]:6379")).toEqual({ host: "::1", port: 6379 });
    expect(parseAddress("node.local:1")).toEqual({
      host: "node.local",
      port: 1,
    });
    expect(
      parseInfo(
        "# header\r\nrole:slave\r\nmaster_node:127.0.0.1:6366\r\ninvalid\r\n",
      ),
    ).toEqual({ role: "slave", master_node: "127.0.0.1:6366" });
    const error = new Error("fixture");
    expect(asError(error)).toBe(error);
    expect(asError(123).message).toBe("123");
  });
  it.each(["missing", "node:0", "node:65536", "host:abc", "::1:6379"])(
    "rejects invalid redirect addresses",
    (address) => {
      expect(() => parseAddress(address)).toThrow();
    },
  );
});
