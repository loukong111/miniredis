import { randomUUID } from "node:crypto";
import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import { join } from "node:path";
import type {
  ConnectionInput,
  ConnectionProfile,
  Theme,
} from "../shared/contracts";

export interface SecretStorage {
  available(): boolean;
  encrypt(value: string): Promise<string>;
  decrypt(value: string): Promise<string>;
}

interface StoredProfile extends Omit<ConnectionProfile, "hasPassword"> {
  secret?: string;
}

export function integer(
  value: unknown,
  name: string,
  minimum: number,
  maximum: number,
): number {
  if (
    typeof value !== "number" ||
    !Number.isInteger(value) ||
    value < minimum ||
    value > maximum
  ) {
    throw new Error(`${name}必须是 ${minimum}–${maximum} 之间的整数`);
  }
  return value;
}

export function validateProfile(
  input: ConnectionInput,
): Omit<StoredProfile, "id" | "secret"> {
  if (!input || typeof input !== "object") throw new Error("连接配置无效");
  const name = typeof input.name === "string" ? input.name.trim() : "";
  const host =
    typeof input.host === "string"
      ? input.host.trim().replace(/^\[([^\]]+)\]$/, "$1")
      : "";
  if (!name || name.length > 80) throw new Error("连接名称需要 1–80 个字符");
  if (!host || host.length > 253 || /[^\w.:-]/.test(host))
    throw new Error("请输入有效的主机名或 IP 地址");
  if (typeof input.username !== "string" || input.username.length > 256)
    throw new Error("用户名无效");
  if (
    input.password !== undefined &&
    (typeof input.password !== "string" || input.password.length > 4096)
  )
    throw new Error("密码长度不能超过 4096 字符");
  return {
    name,
    host,
    username: input.username.trim(),
    port: integer(input.port, "RESP 端口", 1, 65535),
    statsPort: integer(input.statsPort, "监控端口", 1, 65535),
    color: /^#[\da-f]{6}$/i.test(input.color) ? input.color : "#3b82f6",
    followRedirects: input.followRedirects !== false,
  };
}

export class WorkspaceStore {
  private profiles: StoredProfile[] = [];
  private readonly passwords = new Map<string, string>();
  private writeQueue: Promise<void> = Promise.resolve();
  theme: Theme = "light";
  selectedId: string | null = null;

  constructor(
    private readonly directory: string,
    private readonly secrets: SecretStorage,
  ) {}

  async load(): Promise<void> {
    try {
      const parsed: unknown = JSON.parse(
        await readFile(join(this.directory, "workspace.json"), "utf8"),
      );
      if (!parsed || typeof parsed !== "object")
        throw new Error("配置文件格式无效");
      const data = parsed as {
        profiles?: unknown;
        theme?: unknown;
        selectedId?: unknown;
      };
      if (!Array.isArray(data.profiles)) throw new Error("连接列表格式无效");
      const ids = new Set<string>();
      this.profiles = data.profiles.map((value: StoredProfile) => {
        if (typeof value.id !== "string" || !value.id || ids.has(value.id))
          throw new Error("连接 ID 无效");
        ids.add(value.id);
        return {
          ...validateProfile({
            ...value,
            hasPassword: undefined,
          } as ConnectionInput),
          id: value.id,
          secret: typeof value.secret === "string" ? value.secret : undefined,
        };
      });
      this.theme = ["light", "dark", "system"].includes(String(data.theme))
        ? (data.theme as Theme)
        : "light";
      this.selectedId =
        typeof data.selectedId === "string" && ids.has(data.selectedId)
          ? data.selectedId
          : (this.profiles[0]?.id ?? null);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT")
        throw new Error(
          `无法读取工作空间配置，请保留 workspace.json 并检查内容：${(error as Error).message}`,
        );
      this.profiles = [
        {
          id: "local",
          name: "本地开发",
          host: "127.0.0.1",
          port: 6366,
          statsPort: 8080,
          username: "",
          color: "#3b82f6",
          followRedirects: true,
        },
      ];
      this.selectedId = "local";
    }
  }

  list(): ConnectionProfile[] {
    return this.profiles.map(({ secret, ...profile }) => ({
      ...profile,
      hasPassword: Boolean(secret || this.passwords.get(profile.id)),
    }));
  }

  get(id: string): ConnectionProfile {
    const profile = this.list().find((item) => item.id === id);
    if (!profile) throw new Error("连接不存在");
    return profile;
  }

  async password(id: string): Promise<string> {
    this.get(id);
    if (this.passwords.has(id)) return this.passwords.get(id)!;
    const secret = this.profiles.find((profile) => profile.id === id)?.secret;
    if (!secret) return "";
    try {
      if (!this.secrets.available()) throw new Error("密钥环不可用");
      const password = await this.secrets.decrypt(secret);
      this.passwords.set(id, password);
      return password;
    } catch {
      throw new Error("无法访问已保存的密码，请在编辑连接中重新输入");
    }
  }

  async save(input: ConnectionInput): Promise<ConnectionProfile> {
    const profile = validateProfile(input);
    const existing = input.id
      ? this.profiles.find((item) => item.id === input.id)
      : undefined;
    if (input.id && !existing) throw new Error("连接不存在");
    if (!existing && this.profiles.length >= 100)
      throw new Error("最多保存 100 个连接");
    const id = existing?.id ?? randomUUID();
    let secret = existing?.secret;
    if (input.forgetPassword) {
      secret = undefined;
      this.passwords.delete(id);
    }
    if (input.password !== undefined && input.password !== "") {
      this.passwords.set(id, input.password);
      secret = this.secrets.available()
        ? await this.secrets.encrypt(input.password)
        : undefined;
    }
    const next = { ...profile, id, secret };
    if (existing)
      this.profiles = this.profiles.map((item) =>
        item.id === id ? next : item,
      );
    else this.profiles.push(next);
    this.selectedId = id;
    await this.persist();
    return this.get(id);
  }

  async remove(id: string): Promise<void> {
    this.get(id);
    this.profiles = this.profiles.filter((item) => item.id !== id);
    this.passwords.delete(id);
    if (this.selectedId === id) this.selectedId = this.profiles[0]?.id ?? null;
    await this.persist();
  }

  async preferences(input: {
    theme?: Theme;
    selectedId?: string | null;
  }): Promise<void> {
    if (input.theme !== undefined) {
      if (!["light", "dark", "system"].includes(input.theme))
        throw new Error("主题无效");
      this.theme = input.theme;
    }
    if (input.selectedId !== undefined) {
      if (input.selectedId !== null) this.get(input.selectedId);
      this.selectedId = input.selectedId;
    }
    await this.persist();
  }

  private persist(): Promise<void> {
    const body = JSON.stringify(
      {
        version: 1,
        profiles: this.profiles,
        theme: this.theme,
        selectedId: this.selectedId,
      },
      null,
      2,
    );
    const write = async (): Promise<void> => {
      await mkdir(this.directory, { recursive: true, mode: 0o700 });
      const filename = join(this.directory, "workspace.json");
      await writeFile(`${filename}.tmp`, body, { mode: 0o600 });
      await rename(`${filename}.tmp`, filename);
    };
    this.writeQueue = this.writeQueue.then(write, write);
    return this.writeQueue;
  }
}
