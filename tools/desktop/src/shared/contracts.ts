export type Theme = "light" | "dark" | "system";
export type ConnectionStatus =
  "disconnected" | "connecting" | "connected" | "error";

export interface ConnectionProfile {
  id: string;
  name: string;
  host: string;
  port: number;
  statsPort: number;
  username: string;
  color: string;
  followRedirects: boolean;
  hasPassword: boolean;
}

export interface ConnectionInput extends Omit<
  ConnectionProfile,
  "id" | "hasPassword"
> {
  id?: string;
  password?: string;
  forgetPassword?: boolean;
}

export type RespValue =
  | { type: "simple" | "error" | "integer" | "bulk"; value: string }
  | { type: "null" }
  | { type: "array"; value: RespValue[] };

export interface CommandResult {
  id: string;
  runId: string;
  connectionId: string;
  command: string;
  response: RespValue;
  durationMs: number;
  timestamp: number;
  node: string;
  redirects: string[];
  transportError?: boolean;
}

export interface ConnectionState {
  id: string;
  status: ConnectionStatus;
  error?: string;
  connectedAt?: number;
  role?: "primary" | "replica";
}

export type Stats = Record<string, number | string | boolean>;

export interface MonitoringSnapshot {
  connectionId: string;
  timestamp: number;
  stats?: Stats;
  healthy?: boolean;
  ready?: boolean;
  error?: string;
}

export interface ClusterNode {
  id: string;
  address: string;
  role: string;
  state: "healthy" | "suspect" | "failed";
  slots: string;
  current: boolean;
}

export interface TopologySnapshot {
  replication: Record<string, string>;
  nodes: ClusterNode[];
  clusterError?: string;
}

export type LocalMode = "standalone" | "replication" | "cluster" | "failover";
export interface LocalConfig {
  binary: string;
  mode: LocalMode;
  port: number;
  statsPort: number;
  password: string;
  aof: boolean;
  ioThreads: number;
  shards: number;
  memoryMiB: number;
  eviction: "noeviction" | "lru";
}

export interface LocalNode {
  id: string;
  label: string;
  port: number;
  statsPort: number;
  pid?: number;
  state: "starting" | "running" | "stopping" | "stopped" | "error";
  error?: string;
}

export type TaskKind =
  | "benchmark"
  | "smoke"
  | "recovery"
  | "resources"
  | "replication"
  | "cluster"
  | "failover";
export interface TaskInput {
  kind: TaskKind;
  binary: string;
  requests: number;
  clients: number;
  payload: number;
  runs: number;
}

export interface TaskState {
  id: string;
  kind: TaskKind;
  status: "running" | "completed" | "failed" | "cancelled";
  startedAt: number;
  exitCode?: number | null;
}

export interface LogEntry {
  id: string;
  source: string;
  text: string;
  timestamp: number;
}

export interface Workspace {
  profiles: ConnectionProfile[];
  theme: Theme;
  selectedId: string | null;
  binary: string;
  projectRoot: string | null;
  platform: string;
  passwordStorage: "encrypted" | "session";
  version: string;
}

export type DesktopEvent =
  | { type: "connection"; state: ConnectionState }
  | { type: "result"; result: CommandResult }
  | { type: "local"; nodes: LocalNode[] }
  | { type: "task"; task: TaskState }
  | { type: "log"; entry: LogEntry };

export interface DesktopApi {
  workspace(): Promise<Workspace>;
  saveConnection(input: ConnectionInput): Promise<ConnectionProfile>;
  deleteConnection(id: string): Promise<void>;
  preferences(input: {
    theme?: Theme;
    selectedId?: string | null;
  }): Promise<void>;
  connect(id: string): Promise<void>;
  disconnect(id: string): Promise<void>;
  execute(input: {
    connectionId: string;
    script: string;
    runId: string;
  }): Promise<{ count: number; cancelled: boolean }>;
  cancel(id: string): Promise<void>;
  monitoring(id: string): Promise<MonitoringSnapshot>;
  topology(id: string): Promise<TopologySnapshot>;
  metrics(id: string): Promise<string>;
  pickBinary(): Promise<string | null>;
  startLocal(config: LocalConfig): Promise<LocalNode[]>;
  stopLocal(): Promise<void>;
  stopNode(id: string): Promise<void>;
  connectLocal(id: string): Promise<ConnectionProfile>;
  localState(): Promise<LocalNode[]>;
  runTask(input: TaskInput): Promise<TaskState>;
  stopTask(): Promise<void>;
  taskState(): Promise<TaskState | null>;
  logs(): Promise<LogEntry[]>;
  copyText(text: string): Promise<void>;
  exportText(input: { filename: string; content: string }): Promise<boolean>;
  onEvent(callback: (event: DesktopEvent) => void): () => void;
}

declare global {
  interface Window {
    desktop: DesktopApi;
  }
}
