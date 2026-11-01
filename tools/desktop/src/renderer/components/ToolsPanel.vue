<script setup lang="ts">
import { computed, reactive, ref, watch } from "vue";
import {
  NButton,
  NDrawer,
  NDrawerContent,
  NForm,
  NFormItem,
  NInput,
  NInputNumber,
  NPopconfirm,
  NSelect,
  NSwitch,
} from "naive-ui";
import {
  ArrowUpRight,
  CheckCircle2,
  CircleHelp,
  FlaskConical,
  FolderOpen,
  HardDrive,
  LoaderCircle,
  Monitor,
  Moon,
  Play,
  PlugZap,
  Server,
  ShieldCheck,
  Square,
  Sun,
  Terminal,
} from "@lucide/vue";
import type {
  LocalConfig,
  LocalNode,
  TaskInput,
  TaskKind,
  Theme,
} from "../../shared/contracts";
import { errorText, notice, setTheme, state } from "../store";

const localBusy = ref(false);
const taskBusy = ref(false);
const local = reactive<LocalConfig>({
  binary: "",
  mode: "standalone",
  port: 6366,
  statsPort: 8080,
  password: "",
  aof: true,
  ioThreads: 4,
  shards: 16,
  memoryMiB: 64,
  eviction: "lru",
});
const task = reactive<TaskInput>({
  binary: "",
  kind: "benchmark",
  requests: 10000,
  clients: 10,
  payload: 64,
  runs: 3,
});
const taskKinds: { value: TaskKind; label: string; description: string }[] = [
  {
    value: "benchmark",
    label: "性能压测",
    description: "启动隔离实例，多轮测量 SET / GET 吞吐与延迟。",
  },
  {
    value: "smoke",
    label: "协议与接口验证",
    description: "验证命令、认证、TTL、监控与快照恢复。",
  },
  {
    value: "recovery",
    label: "恢复与稳定性",
    description: "验证强杀恢复、损坏快照回退和短时负载。",
  },
  {
    value: "resources",
    label: "资源与故障验证",
    description: "验证内存额度、LRU、AOF 重写与坏尾恢复。",
  },
  {
    value: "replication",
    label: "复制与断线追平",
    description: "验证主从同步、只读保护和 backlog 追平。",
  },
  {
    value: "cluster",
    label: "集群路由验证",
    description: "验证三节点的 slot 映射和命令路由。",
  },
  {
    value: "failover",
    label: "自动故障转移",
    description: "验证多数派选举、旧主降级和写入隔离。",
  },
];
const modeOptions = [
  { label: "单节点", value: "standalone" },
  { label: "主从复制 · 2 节点", value: "replication" },
  { label: "Cluster · 3 节点", value: "cluster" },
  { label: "自动故障转移 · 3 节点", value: "failover" },
];
const liveNodes = computed(() =>
  state.nodes.filter((node) =>
    ["running", "starting", "stopping"].includes(node.state),
  ),
);
const taskDescription = computed(
  () => taskKinds.find((item) => item.value === task.kind)?.description,
);
const taskStatus = computed(() =>
  state.task
    ? {
        running: "运行中",
        completed: "已完成",
        failed: "执行失败",
        cancelled: "已取消",
      }[state.task.status]
    : "尚无运行任务",
);
watch(
  () => state.workspace,
  (workspace) => {
    if (workspace) {
      local.binary ||= workspace.binary;
      task.binary ||= workspace.binary;
    }
  },
  { immediate: true },
);

async function browse(target: "local" | "task"): Promise<void> {
  try {
    const path = await window.desktop.pickBinary();
    if (path) (target === "local" ? local : task).binary = path;
  } catch (error) {
    notice(errorText(error), "error");
  }
}

async function connectNode(node: LocalNode): Promise<void> {
  try {
    const profile = await window.desktop.connectLocal(node.id);
    const index = state.profiles.findIndex((item) => item.id === profile.id);
    if (index >= 0) state.profiles[index] = profile;
    else state.profiles.push(profile);
    state.selectedId = profile.id;
  } catch (error) {
    notice(errorText(error), "error");
  }
}

async function start(): Promise<void> {
  if (localBusy.value) return;
  localBusy.value = true;
  try {
    const nodes = await window.desktop.startLocal({ ...local });
    notice(`已启动 ${nodes.length} 个本地节点`, "success");
    if (nodes[0]) await connectNode(nodes[0]);
  } catch (error) {
    notice(errorText(error), "error");
  } finally {
    localBusy.value = false;
  }
}

async function stop(): Promise<void> {
  try {
    await window.desktop.stopLocal();
    notice("本地实例已停止");
  } catch (error) {
    notice(errorText(error), "error");
  }
}

async function stopNode(id: string): Promise<void> {
  try {
    await window.desktop.stopNode(id);
  } catch (error) {
    notice(errorText(error), "error");
  }
}

async function startTask(): Promise<void> {
  taskBusy.value = true;
  try {
    state.task = await window.desktop.runTask({ ...task });
    state.logOpen = true;
  } catch (error) {
    notice(errorText(error), "error");
  } finally {
    taskBusy.value = false;
  }
}

async function stopTask(): Promise<void> {
  try {
    await window.desktop.stopTask();
  } catch (error) {
    notice(errorText(error), "error");
  }
}
</script>

<template>
  <NDrawer
    :show="!!state.tools"
    :width="600"
    @update:show="!$event && (state.tools = null)"
  >
    <NDrawerContent
      :title="
        state.tools === 'local'
          ? '本地实例'
          : state.tools === 'tasks'
            ? '任务中心'
            : '偏好设置'
      "
      closable
    >
      <template v-if="state.tools === 'local'">
        <div class="tool-intro">
          <div class="dialog-icon"><Server :size="23" /></div>
          <div>
            <h2>为你的工作台启动实例</h2>
            <p>管理由当前客户端启动的本地 MiniRedis 进程。</p>
          </div>
        </div>
        <div v-if="state.workspace?.platform !== 'linux'" class="inline-alert">
          <CircleHelp :size="17" />原生服务端需要
          Linux。当前客户端可直接连接远程 Linux 实例。
        </div>
        <NForm label-placement="top" :show-feedback="false" class="tools-form">
          <NFormItem label="服务端程序"
            ><div class="path-input">
              <NInput
                v-model:value="local.binary"
                placeholder="选择已构建的 miniredis 程序"
                data-testid="local-binary"
              /><NButton @click="browse('local')"
                ><template #icon><FolderOpen :size="16" /></template
              ></NButton></div
          ></NFormItem>
          <NFormItem label="运行模式"
            ><NSelect
              v-model:value="local.mode"
              :options="modeOptions"
              :disabled="localBusy || !!liveNodes.length"
          /></NFormItem>
          <div class="form-row">
            <NFormItem label="起始 RESP 端口"
              ><NInputNumber
                v-model:value="local.port"
                :min="1"
                :max="65533"
                :show-button="false" /></NFormItem
            ><NFormItem label="起始监控端口"
              ><NInputNumber
                v-model:value="local.statsPort"
                :min="1"
                :max="65533"
                :show-button="false"
            /></NFormItem>
          </div>
          <p v-if="local.mode !== 'standalone'" class="field-hint">
            多节点按起始端口递增，所有实例仅监听本机。
          </p>
          <NFormItem label="认证密码（可选）"
            ><NInput
              v-model:value="local.password"
              type="password"
              show-password-on="click"
              placeholder="设置该组节点的共享密码"
          /></NFormItem>
          <div class="form-row">
            <NFormItem label="IO 线程"
              ><NInputNumber
                v-model:value="local.ioThreads"
                :min="1"
                :max="128" /></NFormItem
            ><NFormItem label="缓存分片"
              ><NInputNumber v-model:value="local.shards" :min="1" :max="1024"
            /></NFormItem>
          </div>
          <div class="form-row">
            <NFormItem label="内存额度 / MiB"
              ><NInputNumber
                v-model:value="local.memoryMiB"
                :min="0"
                :max="1048576" /></NFormItem
            ><NFormItem label="淘汰策略"
              ><NSelect
                v-model:value="local.eviction"
                :options="[
                  { label: 'LRU', value: 'lru' },
                  { label: '拒绝超额写入', value: 'noeviction' },
                ]"
            /></NFormItem>
          </div>
          <div class="form-option">
            <span><HardDrive :size="15" />启用 AOF 持久化</span
            ><NSwitch v-model:value="local.aof" size="small" />
          </div>
          <div class="tool-primary-actions">
            <NButton
              type="primary"
              :loading="localBusy"
              :disabled="
                !!liveNodes.length || state.workspace?.platform !== 'linux'
              "
              data-testid="start-local"
              @click="start"
              ><template #icon><Play :size="15" /></template>启动实例</NButton
            ><NButton :disabled="!localBusy && !liveNodes.length" @click="stop"
              ><template #icon><Square :size="13" /></template
              >{{ localBusy ? "取消启动" : "停止全部" }}</NButton
            >
          </div>
        </NForm>
        <div v-if="state.nodes.length" class="owned-nodes">
          <h3>
            当前节点 <span>{{ liveNodes.length }} 个活动进程</span>
          </h3>
          <div v-for="node in state.nodes" :key="node.id" class="owned-node">
            <span
              class="status-dot"
              :class="
                node.state === 'running'
                  ? 'online'
                  : node.state === 'starting' || node.state === 'stopping'
                    ? 'pending'
                    : 'offline'
              "
            />
            <div>
              <strong
                >{{ node.label
                }}<span class="subtle-tag">{{
                  {
                    starting: "启动中",
                    running: "运行中",
                    stopping: "停止中",
                    stopped: "已停止",
                    error: "错误",
                  }[node.state]
                }}</span></strong
              ><code>127.0.0.1:{{ node.port }} · HTTP {{ node.statsPort }}</code
              ><small v-if="node.error" class="danger-text">{{
                node.error
              }}</small>
            </div>
            <button
              class="icon-button"
              :disabled="node.state !== 'running'"
              aria-label="连接此节点"
              title="连接此节点"
              @click="connectNode(node)"
            >
              <PlugZap :size="17" /></button
            ><NPopconfirm @positive-click="stopNode(node.id)"
              ><template #trigger
                ><button
                  class="icon-button"
                  :disabled="node.state !== 'running'"
                  aria-label="停止此节点"
                  title="停止此节点"
                >
                  <Square :size="13" /></button></template
              >停止本地节点 {{ node.port }}？</NPopconfirm
            >
          </div>
        </div>
        <p class="page-footnote">
          数据保存在应用数据目录中。退出客户端会停止它启动的本地进程。
        </p>
      </template>

      <template v-else-if="state.tools === 'tasks'">
        <div class="tool-intro">
          <div class="dialog-icon"><FlaskConical :size="23" /></div>
          <div>
            <h2>用真实负载验证服务</h2>
            <p>执行项目已有的验证脚本，查看完整进程输出。</p>
          </div>
        </div>
        <div v-if="!state.workspace?.projectRoot" class="inline-alert">
          <CircleHelp :size="17" />脚本任务需要从源码目录运行客户端。
        </div>
        <NForm label-placement="top" :show-feedback="false" class="tools-form"
          ><NFormItem label="服务端程序"
            ><div class="path-input">
              <NInput
                data-testid="task-binary"
                v-model:value="task.binary"
                placeholder="miniredis 可执行文件"
              /><NButton @click="browse('task')"
                ><template #icon><FolderOpen :size="16" /></template
              ></NButton></div></NFormItem
          ><NFormItem label="任务类型"
            ><NSelect
              v-model:value="task.kind"
              :options="taskKinds"
              :disabled="state.task?.status === 'running'"
          /></NFormItem>
          <p class="field-hint">{{ taskDescription }}</p>
          <div
            v-if="['benchmark', 'recovery', 'resources'].includes(task.kind)"
            class="task-parameters"
          >
            <div class="form-row">
              <NFormItem label="请求数"
                ><NInputNumber
                  data-testid="task-requests"
                  v-model:value="task.requests"
                  :min="1"
                  :max="1000000" /></NFormItem
              ><NFormItem label="客户端数"
                ><NInputNumber
                  data-testid="task-clients"
                  v-model:value="task.clients"
                  :min="1"
                  :max="1000"
              /></NFormItem>
            </div>
            <div v-if="task.kind === 'benchmark'" class="form-row">
              <NFormItem label="Value 字节数"
                ><NInputNumber
                  data-testid="task-payload"
                  v-model:value="task.payload"
                  :min="1"
                  :max="1048576" /></NFormItem
              ><NFormItem label="轮数"
                ><NInputNumber
                  data-testid="task-runs"
                  v-model:value="task.runs"
                  :min="1"
                  :max="5"
              /></NFormItem>
            </div>
          </div>
          <div class="tool-primary-actions">
            <NButton
              type="primary"
              :loading="taskBusy"
              :disabled="
                state.task?.status === 'running' ||
                !state.workspace?.projectRoot ||
                state.workspace.platform !== 'linux'
              "
              @click="startTask"
              ><template #icon><Play :size="15" /></template>运行任务</NButton
            ><NButton
              :disabled="state.task?.status !== 'running'"
              @click="stopTask"
              ><template #icon><Square :size="13" /></template>取消任务</NButton
            >
          </div></NForm
        >
        <section class="task-status-card">
          <div class="task-status-icon">
            <LoaderCircle
              v-if="state.task?.status === 'running'"
              :size="23"
              class="spinning"
            /><CheckCircle2
              v-else-if="state.task?.status === 'completed'"
              :size="23"
              class="success-text"
            /><Terminal v-else :size="23" />
          </div>
          <div>
            <h3>{{ taskStatus }}</h3>
            <p>
              {{
                state.task
                  ? taskKinds.find((kind) => kind.value === state.task?.kind)
                      ?.label
                  : "选择一项任务开始验证"
              }}
            </p>
            <small v-if="state.task">{{
              new Date(state.task.startedAt).toLocaleString("zh-CN", {
                hour12: false,
              })
            }}</small>
          </div>
        </section>
        <div class="task-log-preview">
          <header>
            <h3>最近输出</h3>
            <button
              class="text-button"
              @click="
                state.logOpen = true;
                state.tools = null;
              "
            >
              展开日志<ArrowUpRight :size="14" />
            </button>
          </header>
          <pre>{{
            state.logs
              .filter((item) => item.source === state.task?.kind)
              .slice(-15)
              .map((item) => item.text)
              .join("\n") || "任务开始后，输出将显示在这里。"
          }}</pre>
        </div>
      </template>

      <template v-else>
        <div class="tool-intro">
          <div class="dialog-icon"><Monitor :size="23" /></div>
          <div>
            <h2>让工作台更顺手</h2>
            <p>外观设置会保存在当前设备。</p>
          </div>
        </div>
        <h3 class="settings-heading">外观主题</h3>
        <div class="theme-options">
          <button
            v-for="option in [
              { key: 'light', label: '浅色', icon: Sun },
              { key: 'dark', label: '深色', icon: Moon },
              { key: 'system', label: '跟随系统', icon: Monitor },
            ]"
            :key="option.key"
            :class="{ selected: state.theme === option.key }"
            @click="setTheme(option.key as Theme)"
          >
            <component :is="option.icon" :size="24" /><span>{{
              option.label
            }}</span>
          </button>
        </div>
        <section class="settings-note">
          <ShieldCheck :size="21" />
          <div>
            <h3>连接与密码</h3>
            <p>
              {{
                state.workspace?.passwordStorage === "encrypted"
                  ? "连接密码由系统密钥环加密保存。"
                  : "当前环境没有可用的安全密钥环。连接信息会保存，密码仅保留在本次会话。"
              }}
            </p>
          </div>
        </section>
        <section class="settings-note">
          <Terminal :size="21" />
          <div>
            <h3>键盘操作</h3>
            <p>
              Ctrl / ⌘ + Enter：执行选中内容或当前行。<br />Ctrl / ⌘ + Shift +
              Enter：执行全部命令。<br />命令历史仅在本次会话保留，不记录 AUTH
              命令。
            </p>
          </div>
        </section>
        <section class="settings-note">
          <Server :size="21" />
          <div>
            <h3>MiniRedis Desktop</h3>
            <p>
              版本 {{ state.workspace?.version }} · 客户端可连接远程 Linux
              服务端。<br />本地进程和验证脚本需要 Linux 环境。
            </p>
          </div>
        </section>
      </template>
    </NDrawerContent>
  </NDrawer>
</template>
