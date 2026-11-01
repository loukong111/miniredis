import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { WorkspaceStore, type SecretStorage } from "../src/main/workspace";

const directories: string[] = [];
const memory: SecretStorage = {
  available: () => false,
  encrypt: async () => {
    throw new Error("must not encrypt");
  },
  decrypt: async () => {
    throw new Error("must not decrypt");
  },
};
async function directory(): Promise<string> {
  const path = await mkdtemp(join(tmpdir(), "miniredis-workspace-"));
  directories.push(path);
  return path;
}
afterEach(async () => {
  await Promise.all(
    directories
      .splice(0)
      .map((path) => rm(path, { recursive: true, force: true })),
  );
});

describe("workspace persistence", () => {
  it("never writes plaintext passwords when a keyring is unavailable", async () => {
    const path = await directory();
    const store = new WorkspaceStore(path, memory);
    await store.load();
    const profile = await store.save({
      ...store.get("local"),
      password: "test-password-unpersisted",
    });
    expect(await store.password(profile.id)).toBe("test-password-unpersisted");
    expect(JSON.stringify(store.list())).not.toContain(
      "test-password-unpersisted",
    );
    expect(await readFile(join(path, "workspace.json"), "utf8")).not.toContain(
      "test-password-unpersisted",
    );
    const reload = new WorkspaceStore(path, memory);
    await reload.load();
    expect(await reload.password(profile.id)).toBe("");
  });
  it("persists encrypted secrets, keeps passwords on blank edits, and can remove them", async () => {
    const path = await directory();
    const secrets: SecretStorage = {
      available: () => true,
      encrypt: async (value) => Buffer.from(value).toString("base64"),
      decrypt: async (value) => Buffer.from(value, "base64").toString(),
    };
    const store = new WorkspaceStore(path, secrets);
    await store.load();
    await store.save({ ...store.get("local"), password: "fixture-secret" });
    expect(await readFile(join(path, "workspace.json"), "utf8")).not.toContain(
      "fixture-secret",
    );
    const reload = new WorkspaceStore(path, secrets);
    await reload.load();
    await reload.save({ ...reload.get("local"), name: "Edited", password: "" });
    expect(await reload.password("local")).toBe("fixture-secret");
    await reload.save({ ...reload.get("local"), forgetPassword: true });
    expect(await reload.password("local")).toBe("");
    expect(reload.get("local").hasPassword).toBe(false);
  });
  it("rejects invalid profiles and preserves corrupt configuration for recovery", async () => {
    const path = await directory();
    const store = new WorkspaceStore(path, memory);
    await store.load();
    await expect(
      store.save({ ...store.get("local"), port: 0 }),
    ).rejects.toThrow("RESP");
    await expect(
      store.save({ ...store.get("local"), host: "http://localhost/" }),
    ).rejects.toThrow("主机");
    await writeFile(join(path, "workspace.json"), "{broken");
    await expect(new WorkspaceStore(path, memory).load()).rejects.toThrow(
      "保留 workspace.json",
    );
    expect(await readFile(join(path, "workspace.json"), "utf8")).toBe(
      "{broken",
    );
  });
});
