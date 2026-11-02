<script setup lang="ts">
import { computed, nextTick, onMounted, ref, watch } from "vue";
import {
  NButton,
  NConfigProvider,
  NDropdown,
  NModal,
  NSwitch,
  darkTheme,
  zhCN,
  dateZhCN,
  type GlobalThemeOverrides,
} from "naive-ui";
import {
  Activity,
  ArrowDownToLine,
  ArrowUpRight,
  Cable,
  CheckCircle2,
  CircleAlert,
  CircleHelp,
  Database,
  Ellipsis,
  FlaskConical,
  GitBranch,
  LoaderCircle,
  Moon,
  PanelBottom,
  Plus,
  RefreshCw,
  Search,
  Server,
  Settings2,
  SquareTerminal,
  Sun,
  Terminal,
  Unplug,
  X,
} from "@lucide/vue";
import type { ConnectionProfile, Theme } from "../shared/contracts";
import ConnectionDialog from "./components/ConnectionDialog.vue";
import CommandWorkbench from "./components/CommandWorkbench.vue";
import MonitorPanel from "./components/MonitorPanel.vue";
import TopologyPanel from "./components/TopologyPanel.vue";
import ToolsPanel from "./components/ToolsPanel.vue";
import {
  active,
  bytes,
  connected,
  connectProfile,
  connection,
  errorText,
  initialize,
  forgetConnection,
  isDark,
  notice,
  number,
  refreshMonitoring,
  setTheme,
  snapshot,
  state,
  stats,
} from "./store";

const search = ref("");
const connectionDialog = ref(false);
const editing = ref<ConnectionProfile | null>(null);
const deleting = ref<ConnectionProfile | null>(null);
const deletingBusy = ref(false);
const logElement = ref<HTMLElement>();
const filteredProfiles = computed(() =>
  state.profiles.filter((profile) =>
    `${profile.name} ${profile.host} ${profile.port}`
      .toLowerCase()
      .includes(search.value.toLowerCase()),
  ),
);
const pageTitle = computed(
  () =>
    ({ workbench: "工作台", monitoring: "运行监控", topology: "复制与集群" })[
      state.page
    ],
);
const connectionLabel = computed(
  () =>
    ({
      connected: "已连接",
      connecting: "连接中",
      error: "连接失败",
      disconnected: "未连接",
    })[connection.value?.status ?? "disconnected"],
);
const liveNodes = computed(
  () => state.nodes.filter((node) => node.state === "running").length,
);
const themeOverrides = computed<GlobalThemeOverrides>(() => ({
  common: {
    primaryColor: isDark.value ? "#80a8ff" : "#4275ee",
    primaryColorHover: isDark.value ? "#a1bfff" : "#3567dc",
    primaryColorPressed: "#2f5ecc",
    borderRadius: "7px",
    fontFamily:
      'Inter, "Noto Sans CJK SC", "Microsoft YaHei", system-ui, sans-serif',
    fontSize: "13px",
    bodyColor: isDark.value ? "#111722" : "#f6f8fb",
    cardColor: isDark.value ? "#171d28" : "#fff",
    modalColor: isDark.value ? "#171d28" : "#fff",
  },
  Button: { fontWeight: "500", heightSmall: "30px", paddingSmall: "0 12px" },
  Input: { heightMedium: "38px", color: isDark.value ? "#1b2331" : "#fff" },
  InputNumber: { heightMedium: "38px" },
  Select: { peers: { InternalSelection: { heightMedium: "38px" } } },
}));

function openConnection(profile: ConnectionProfile | null = null): void {
  editing.value = profile;
  connectionDialog.value = true;
}
function profileMenu(key: string, profile: ConnectionProfile): void {
  if (key === "edit") openConnection(profile);
  else deleting.value = profile;
}
async function disconnect(): Promise<void> {
  if (active.value)
    try {
      await window.desktop.disconnect(active.value.id);
    } catch (error) {
      notice(errorText(error), "error");
    }
}
async function deleteConnection(): Promise<void> {
  if (!deleting.value || deletingBusy.value) return;
  deletingBusy.value = true;
  try {
    const id = deleting.value.id;
    await window.desktop.deleteConnection(id);
    forgetConnection(id);
    deleting.value = null;
    notice("连接已删除");
  } catch (error) {
    notice(errorText(error), "error");
  } finally {
    deletingBusy.value = false;
  }
}
async function exportLogs(): Promise<void> {
  try {
    await window.desktop.exportText({
      filename: "miniredis-session.log",
      content: state.logs
        .map(
          (entry) =>
            `${new Date(entry.timestamp).toISOString()} [${entry.source}] ${entry.text}`,
        )
        .join("\n"),
    });
  } catch (error) {
    notice(errorText(error), "error");
  }
}
watch(
  () => state.logs.length + ":" + state.logs.at(-1)?.id,
  async () => {
    await nextTick();
    if (logElement.value)
      logElement.value.scrollTop = logElement.value.scrollHeight;
  },
);
onMounted(initialize);
</script>

<template>
  <NConfigProvider
    :locale="zhCN"
    :date-locale="dateZhCN"
    :theme="isDark ? darkTheme : null"
    :theme-overrides="themeOverrides"
  >
    <div class="desktop-shell">
      <nav class="activity-rail" aria-label="主导航">
        <div class="brand-mark" title="MiniRedis Desktop">
          <svg
            width="31"
            height="31"
            viewBox="0 0 32 32"
            fill="none"
            aria-label="MiniRedis"
          >
            <path
              d="M5 20 16 26 27 20V24L16 30 5 24Z"
              fill="currentColor"
              opacity=".45"
            />
            <path
              d="M5 13 16 19 27 13V17L16 23 5 17Z"
              fill="currentColor"
              opacity=".7"
            />
            <path d="m16 2 12 6.5L16 15 4 8.5Z" fill="currentColor" />
          </svg>
        </div>
        <div class="rail-pages">
          <button
            class="rail-button"
            :class="{ selected: !state.tools }"
            aria-label="工作空间"
            title="工作空间"
            @click="state.tools = null"
          >
            <Database :size="21" />
          </button>
          <button
            class="rail-button"
            :class="{ selected: state.tools === 'local' }"
            title="本地实例"
            aria-label="本地实例"
            data-testid="open-local"
            @click="state.tools = 'local'"
          >
            <Server :size="20" /><i
              v-if="liveNodes"
              class="rail-notification"
            /></button
          ><button
            class="rail-button"
            :class="{ selected: state.tools === 'tasks' }"
            title="任务中心"
            aria-label="任务中心"
            @click="state.tools = 'tasks'"
          >
            <FlaskConical :size="20" /><i
              v-if="state.task?.status === 'running'"
              class="rail-notification amber"
            />
          </button>
        </div>
        <div class="rail-bottom">
          <button
            class="rail-button"
            :class="{ selected: state.tools === 'settings' }"
            title="设置"
            aria-label="设置"
            @click="state.tools = 'settings'"
          >
            <Settings2 :size="20" />
          </button>
        </div>
      </nav>

      <aside class="connection-sidebar">
        <header class="sidebar-brand">
          <strong>MiniRedis<span>Desktop</span></strong
          ><span class="workspace-badge">工作空间</span>
        </header>
        <div class="sidebar-search">
          <Search :size="15" /><input
            v-model="search"
            placeholder="搜索连接…"
            aria-label="搜索连接"
          /><kbd>⌕</kbd>
        </div>
        <div class="sidebar-section">
          <span>我的连接</span
          ><span class="sidebar-count">{{ state.profiles.length }}</span
          ><button
            class="icon-button compact"
            aria-label="添加连接"
            title="添加连接"
            data-testid="add-connection"
            @click="openConnection()"
          >
            <Plus :size="16" />
          </button>
        </div>
        <div class="connection-list">
          <div
            v-for="profile in filteredProfiles"
            :key="profile.id"
            class="connection-item"
            :class="{ selected: state.selectedId === profile.id }"
          >
            <button
              class="connection-select"
              :aria-label="'选择连接 ' + profile.name"
              @click="state.selectedId = profile.id"
              @dblclick="connectProfile(profile.id)"
            >
              <div
                class="connection-icon"
                :style="{ '--connection-color': profile.color }"
              >
                <Database :size="19" /><span
                  class="connection-presence"
                  :class="
                    state.connections[profile.id]?.status || 'disconnected'
                  "
                />
              </div>
              <span class="connection-caption"
                ><strong>{{ profile.name }}</strong
                ><code>{{ profile.host }}:{{ profile.port }}</code></span
              ></button
            ><NDropdown
              trigger="click"
              :options="[
                { label: '编辑连接', key: 'edit' },
                { label: '删除连接', key: 'delete' },
              ]"
              @select="(key: string) => profileMenu(key, profile)"
              ><button
                class="icon-button compact connection-more"
                :aria-label="'管理连接 ' + profile.name"
              >
                <Ellipsis :size="16" /></button
            ></NDropdown>
          </div>
          <p v-if="search && !filteredProfiles.length" class="sidebar-empty">
            没有找到匹配的连接
          </p>
          <button class="add-connection" @click="openConnection()">
            <Plus :size="15" />新建连接
          </button>
        </div>
        <div class="sidebar-tip">
          <div class="tip-icon"><Terminal :size="18" /></div>
          <strong>从一条 PING 开始</strong>
          <p>连接实例后，即可运行命令、<br />观察指标与检查复制状态。</p>
        </div>
        <footer class="sidebar-footer">
          <span class="status-dot online" /><span>本地工作空间</span
          ><span>v{{ state.workspace?.version || "1.0.0" }}</span>
        </footer>
      </aside>

      <div class="main-shell">
        <header class="workspace-header">
          <div class="workspace-location">
            <span class="breadcrumb">工作空间</span
            ><span class="breadcrumb-slash">/</span
            ><Database :size="16" /><strong>{{
              active?.name || "未选择连接"
            }}</strong
            ><span
              class="connection-badge"
              :class="connection?.status || 'disconnected'"
              ><span
                class="status-dot"
                :class="
                  connected
                    ? 'online'
                    : connection?.status === 'connecting'
                      ? 'pending'
                      : connection?.status === 'error'
                        ? 'failed'
                        : 'offline'
                "
              />{{ connectionLabel }}</span
            >
            <span
              v-if="connected && connection?.role"
              class="workspace-badge"
              >{{ connection.role === "primary" ? "Primary" : "Replica" }}</span
            >
          </div>
          <div class="header-actions">
            <button
              class="icon-button"
              :disabled="!connected"
              title="刷新监控"
              aria-label="刷新监控"
              :class="{ spinning: state.monitorLoading }"
              @click="!state.monitorLoading && refreshMonitoring()"
            >
              <RefreshCw :size="17" /></button
            ><span class="toolbar-divider" /><button
              class="icon-button"
              :aria-label="isDark ? '切换浅色主题' : '切换深色主题'"
              :title="isDark ? '浅色主题' : '深色主题'"
              data-testid="theme-toggle"
              @click="setTheme(isDark ? 'light' : 'dark')"
            >
              <Sun v-if="isDark" :size="18" /><Moon v-else :size="18" /></button
            ><NButton v-if="connected" size="small" @click="disconnect"
              ><template #icon><Unplug :size="14" /></template>断开</NButton
            ><NButton
              v-else
              type="primary"
              size="small"
              :loading="connection?.status === 'connecting'"
              :disabled="!active"
              data-testid="connect-current"
              @click="active && connectProfile(active.id)"
              ><template #icon><Cable :size="15" /></template>连接实例</NButton
            >
          </div>
        </header>

        <nav class="workspace-tabs" aria-label="当前连接页面">
          <button
            v-for="page in [
              { id: 'workbench', label: '命令工作台' },
              { id: 'monitoring', label: '运行监控' },
              { id: 'topology', label: '复制与集群' },
            ]"
            :key="page.id"
            :class="{ selected: state.page === page.id }"
            :data-testid="'nav-' + page.id"
            @click="state.page = page.id as typeof state.page"
          >
            <SquareTerminal
              v-if="page.id === 'workbench'"
              :size="16"
            /><Activity
              v-else-if="page.id === 'monitoring'"
              :size="16"
            /><GitBranch v-else :size="16" />{{ page.label
            }}<span v-if="page.id === 'workbench'" class="tab-shortcut"
              >⌘ ↵</span
            >
          </button>
          <div class="tab-spacer" />
          <label class="auto-refresh"
            >自动刷新<NSwitch v-model:value="state.autoRefresh" size="small"
          /></label>
        </nav>

        <main class="workspace-content">
          <div v-if="state.bootError" class="empty-state boot-error">
            <CircleAlert :size="38" />
            <h2>无法打开工作空间</h2>
            <p>{{ state.bootError }}</p>
          </div>
          <div v-else-if="!state.ready" class="empty-state">
            <LoaderCircle :size="28" class="spinning" />
            <p>正在准备工作空间…</p>
          </div>
          <div v-else-if="!active" class="empty-state welcome-state">
            <div class="welcome-logo"><Database :size="35" /></div>
            <div class="eyebrow">YOUR DATA, YOUR WORKSPACE</div>
            <h1>你好，MiniRedis。</h1>
            <p>添加你的第一个连接，开始探索数据与服务。</p>
            <NButton type="primary" @click="openConnection()"
              ><template #icon><Plus :size="16" /></template>添加连接</NButton
            ><button class="text-button" @click="state.tools = 'local'">
              或者先启动本地实例<ArrowUpRight :size="14" />
            </button>
          </div>
          <template v-else>
            <section v-show="state.page === 'workbench'" class="workbench-page">
              <header class="page-heading">
                <div>
                  <div class="eyebrow">DEVELOPER WORKSPACE</div>
                  <h1>命令工作台</h1>
                  <p>让每一次请求，都有清晰的回应。</p>
                </div>
                <div class="workspace-summary">
                  <div>
                    <span>Key 数量</span
                    ><strong>{{ number(stats?.key_count) }}</strong>
                  </div>
                  <div>
                    <span>数据内存</span
                    ><strong>{{ bytes(stats?.used_memory_bytes) }}</strong>
                  </div>
                  <div>
                    <span>当前连接</span
                    ><strong>{{ number(stats?.connected_clients) }}</strong>
                  </div>
                </div>
              </header>
              <div
                v-if="connection?.status === 'error'"
                class="inline-alert error"
              >
                <CircleAlert :size="16" /><span>{{ connection.error }}</span
                ><button
                  class="text-button"
                  @click="connectProfile(active!.id)"
                >
                  重新连接<ArrowUpRight :size="13" />
                </button>
              </div>
              <CommandWorkbench />
            </section>
            <MonitorPanel v-show="state.page === 'monitoring'" />
            <TopologyPanel v-show="state.page === 'topology'" />
          </template>
        </main>

        <section v-if="state.logOpen" class="log-dock">
          <header>
            <div>
              <Terminal :size="14" /><strong>进程与任务日志</strong
              ><span class="count-badge">{{ state.logs.length }}</span>
            </div>
            <div>
              <button
                class="icon-button compact"
                :disabled="!state.logs.length"
                aria-label="导出日志"
                title="导出日志"
                @click="exportLogs"
              >
                <ArrowDownToLine :size="14" /></button
              ><button class="text-button small" @click="state.logs = []">
                清空视图</button
              ><button
                class="icon-button compact"
                aria-label="关闭日志"
                @click="state.logOpen = false"
              >
                <X :size="15" />
              </button>
            </div>
          </header>
          <div ref="logElement" class="log-lines">
            <p v-if="!state.logs.length" class="muted">
              启动本地实例或运行任务后，日志会显示在这里。
            </p>
            <div v-for="entry in state.logs" :key="entry.id">
              <time>{{
                new Date(entry.timestamp).toLocaleTimeString("zh-CN", {
                  hour12: false,
                })
              }}</time
              ><span class="log-source">{{ entry.source }}</span
              ><span>{{ entry.text }}</span>
            </div>
          </div>
        </section>

        <footer class="workspace-footer">
          <div>
            <span
              class="status-dot"
              :class="connected ? 'online' : 'offline'"
            /><span>{{
              connected ? `${active?.host}:${active?.port}` : "尚未建立连接"
            }}</span
            ><span v-if="connected" class="footer-separator" /><span
              v-if="connected"
              >HTTP {{ active?.statsPort }}</span
            ><span v-if="snapshot?.stats" class="footer-updated"
              >已更新
              {{
                new Date(snapshot.timestamp).toLocaleTimeString("zh-CN", {
                  hour12: false,
                })
              }}</span
            >
          </div>
          <div>
            <button
              v-if="liveNodes"
              class="text-button small"
              @click="state.tools = 'local'"
            >
              <Server :size="13" />{{ liveNodes }} 个本地节点</button
            ><button
              class="text-button small"
              :class="{ active: state.logOpen }"
              @click="state.logOpen = !state.logOpen"
            >
              <PanelBottom :size="14" />日志<span
                v-if="state.task?.status === 'running'"
                class="pulse-dot"
              />
            </button>
          </div>
        </footer>
      </div>
      <ConnectionDialog v-model:show="connectionDialog" :profile="editing" />
      <ToolsPanel />
      <NModal
        :show="!!deleting"
        :mask-closable="!deletingBusy"
        @update:show="!$event && (deleting = null)"
        ><div
          class="dialog small-dialog"
          role="dialog"
          aria-modal="true"
          aria-labelledby="delete-connection-title"
        >
          <div class="dialog-icon danger"><CircleAlert :size="23" /></div>
          <h2 id="delete-connection-title">删除连接？</h2>
          <p class="muted">
            将移除「{{
              deleting?.name
            }}」及其保存的密码。实例中的数据不会被删除。
          </p>
          <footer class="dialog-actions">
            <NButton :disabled="deletingBusy" @click="deleting = null"
              >取消</NButton
            ><NButton
              type="error"
              :loading="deletingBusy"
              @click="deleteConnection"
              >删除连接</NButton
            >
          </footer>
        </div></NModal
      >
      <div class="toast-stack" aria-live="polite">
        <div
          v-for="item in state.notices"
          :key="item.id"
          class="toast"
          :class="item.type"
        >
          <CheckCircle2 v-if="item.type === 'success'" :size="17" /><CircleAlert
            v-else-if="item.type === 'error'"
            :size="17"
          /><CircleHelp v-else :size="17" /><span>{{ item.message }}</span
          ><button
            class="icon-button compact"
            aria-label="关闭通知"
            @click="
              state.notices = state.notices.filter(
                (notice) => notice.id !== item.id,
              )
            "
          >
            <X :size="13" />
          </button>
        </div>
      </div>
    </div>
  </NConfigProvider>
</template>
