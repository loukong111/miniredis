import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import {
  WorkspaceStore,
  validateProfile,
  type SecretStorage,
} from "../src/main/workspace";

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
  it.each(["save", "remove", "preferences"] as const)(
    "preserves committed state if %s cannot be persisted",
    async (operation) => {
      const path = await directory();
      const store = new WorkspaceStore(path, memory);
      await store.load();
      await store.save({ ...store.get("local"), password: "original-fixture" });
      const before = store.list();
      await rm(join(path, "workspace.json"));
      await mkdir(join(path, "workspace.json"));
      const result =
        operation === "save"
          ? store.save({
              ...store.get("local"),
              name: "must not commit",
              password: "new-fixture",
            })
          : operation === "remove"
            ? store.remove("local")
            : store.preferences({ theme: "dark", selectedId: null });
      await expect(result).rejects.toThrow();
      expect(store.list()).toEqual(before);
      expect(await store.password("local")).toBe("original-fixture");
      expect(store.theme).toBe("light");
      expect(store.selectedId).toBe("local");
      await rm(join(path, "workspace.json"), { recursive: true });
      await store.preferences({ theme: "dark" });
      expect(store.theme).toBe("dark");
    },
  );
  it("serializes overlapping password edits so memory and disk retain the last requested value", async () => {
    const path = await directory();
    const secrets: SecretStorage = {
      available: () => true,
      encrypt: async (value) => {
        if (value === "first-fixture")
          await new Promise((done) => setTimeout(done, 40));
        return Buffer.from(value).toString("base64");
      },
      decrypt: async (value) => Buffer.from(value, "base64").toString(),
    };
    const store = new WorkspaceStore(path, secrets);
    await store.load();
    const profile = store.get("local");
    await Promise.all([
      store.save({ ...profile, password: "first-fixture" }),
      store.save({ ...profile, password: "second-fixture" }),
    ]);
    const restored = new WorkspaceStore(path, secrets);
    await restored.load();
    expect(await store.password("local")).toBe("second-fixture");
    expect(await restored.password("local")).toBe("second-fixture");
  });
  it("does not change cached credentials if encryption fails", async () => {
    const path = await directory();
    let fail = false;
    const secrets: SecretStorage = {
      available: () => true,
      encrypt: async (value) => {
        if (fail) throw new Error("keyring failed");
        return Buffer.from(value).toString("base64");
      },
      decrypt: async (value) => Buffer.from(value, "base64").toString(),
    };
    const store = new WorkspaceStore(path, secrets);
    await store.load();
    await store.save({ ...store.get("local"), password: "old-fixture" });
    fail = true;
    await expect(
      store.save({ ...store.get("local"), password: "new-fixture" }),
    ).rejects.toThrow("keyring failed");
    expect(await store.password("local")).toBe("old-fixture");
  });
  it("validates limits, selections, missing profiles and configuration permissions", async () => {
    const path = await directory();
    const store = new WorkspaceStore(path, memory);
    await store.load();
    expect(() => store.get("absent")).toThrow("不存在");
    await expect(
      store.save({ ...store.get("local"), id: "absent" }),
    ).rejects.toThrow("不存在");
    await expect(store.remove("absent")).rejects.toThrow("不存在");
    await expect(
      store.preferences({ theme: "invalid" as never }),
    ).rejects.toThrow("主题");
    await expect(store.preferences({ selectedId: "absent" })).rejects.toThrow(
      "不存在",
    );
    await store.preferences({ theme: "system", selectedId: null });
    expect(store.selectedId).toBeNull();
    expect((await stat(join(path, "workspace.json"))).mode & 0o777).toBe(0o600);
    await writeFile(
      join(path, "workspace.json"),
      JSON.stringify({
        profiles: Array.from({ length: 100 }, (_, i) => ({
          ...store.get("local"),
          id: String(i),
        })),
      }),
    );
    const full = new WorkspaceStore(path, memory);
    await full.load();
    const { id: _id, ...input } = full.list()[0];
    await expect(full.save(input)).rejects.toThrow("100");
  });
  it("reports encrypted secrets that cannot be decrypted without exposing them", async () => {
    const path = await directory();
    const store = new WorkspaceStore(path, memory);
    await store.load();
    await writeFile(
      join(path, "workspace.json"),
      JSON.stringify({
        profiles: [{ ...store.get("local"), secret: "encrypted-fixture" }],
      }),
    );
    const restored = new WorkspaceStore(path, memory);
    await restored.load();
    await expect(restored.password("local")).rejects.toThrow("重新输入");
  });
  it.each([
    { name: "" },
    { name: "x".repeat(81) },
    { host: "" },
    { host: "http://host" },
    { username: "x".repeat(257) },
    { password: "x".repeat(4097) },
    { port: 65536 },
    { port: 1.5 },
    { statsPort: 0 },
  ])("rejects invalid profile fields", async (changes) => {
    expect(() =>
      validateProfile({
        name: "profile",
        host: "localhost",
        port: 1,
        statsPort: 2,
        username: "",
        color: "#123456",
        followRedirects: true,
        ...changes,
      }),
    ).toThrow();
  });
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
