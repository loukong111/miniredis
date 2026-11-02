<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref, watch } from "vue";
import {
  NButton,
  NForm,
  NFormItem,
  NInput,
  NInputNumber,
  NModal,
  NPopconfirm,
} from "naive-ui";
import {
  ArrowRight,
  CircleHelp,
  Crown,
  GitBranch,
  Layers3,
  Link2,
  MoveRight,
  Network,
  Plus,
  RefreshCw,
  Server,
  X,
} from "@lucide/vue";
import type { TopologySnapshot } from "../../shared/contracts";
import {
  active,
  connected,
  errorText,
  execute,
  notice,
  number,
  state,
  stats,
} from "../store";

const topology = ref<TopologySnapshot | null>(null);
const loading = ref(false);
const error = ref("");
const operation = ref<"meet" | "migrate" | null>(null);
const form = reactive({ target: "", slot: 0 });
let timer: ReturnType<typeof setTimeout> | undefined;
let generation = 0;
const role = computed(() => topology.value?.replication.role);
const isReplica = computed(
  () => role.value === "slave" || role.value === "replica",
);
const roleName = computed(() =>
  role.value === "master"
    ? "Primary"
    : isReplica.value
      ? "Replica"
      : "未知角色",
);

async function refresh(): Promise<void> {
  clearTimeout(timer);
  const token = ++generation;
  const id = active.value?.id;
  if (!connected.value || !id || state.page !== "topology") {
    if (!connected.value || !id) topology.value = null;
    loading.value = false;
    return;
  }
  loading.value = true;
  error.value = "";
  try {
    const value = await window.desktop.topology(id);
    if (token === generation) topology.value = value;
  } catch (cause) {
    if (token === generation) {
      error.value = errorText(cause);
      topology.value = null;
    }
  } finally {
    if (token === generation) {
      loading.value = false;
      if (state.autoRefresh) timer = setTimeout(refresh, 5000);
    }
  }
}

async function submit(): Promise<void> {
  if (!form.target.trim()) {
    notice("请输入目标节点地址", "error");
    return;
  }
  const script =
    operation.value === "meet"
      ? `CLUSTER MEET ${JSON.stringify(form.target.trim())}`
      : `CLUSTER MIGRATE ${form.slot} ${JSON.stringify(form.target.trim())}`;
  operation.value = null;
  state.page = "workbench";
  await execute(script);
}

let viewedId: string | null = null;
watch(
  [
    () => state.selectedId,
    () => state.page,
    connected,
    () => state.autoRefresh,
  ],
  () => {
    if (state.selectedId !== viewedId) {
      topology.value = null;
      viewedId = state.selectedId;
    }
    void refresh();
  },
  { immediate: true },
);
onBeforeUnmount(() => {
  generation++;
  clearTimeout(timer);
});
</script>

<template>
  <div class="page-scroll topology-page">
    <header class="page-heading">
      <div>
        <div class="eyebrow">REPLICATION & CLUSTER</div>
        <h1>复制与集群</h1>
        <p>检查节点角色、复制进度和槽位归属。</p>
      </div>
      <NButton
        size="small"
        :loading="loading"
        :disabled="!connected"
        @click="refresh"
        ><template #icon><RefreshCw :size="15" /></template>刷新拓扑</NButton
      >
    </header>
    <div v-if="error || !connected" class="inline-alert">
      <CircleHelp :size="17" />{{ error || "连接实例后查看复制与集群状态。" }}
    </div>
    <section class="panel replication-overview">
      <div class="node-portrait"><Server :size="30" /></div>
      <div class="node-identity">
        <span class="eyebrow">当前节点</span>
        <h2>
          {{ active?.name || "未选择连接"
          }}<span class="role-tag" :class="isReplica ? 'purple' : ''">{{
            roleName
          }}</span>
        </h2>
        <code>{{ active?.host }}:{{ active?.port }}</code>
      </div>
      <div class="replication-stat">
        <span>复制 Offset</span
        ><strong>{{
          topology?.replication.master_repl_offset ||
          topology?.replication.slave_repl_offset ||
          "—"
        }}</strong>
      </div>
      <div class="replication-stat">
        <span>在线副本</span
        ><strong
          >{{ number(stats?.replication_connected_replicas)
          }}<small>
            / {{ number(stats?.replication_configured_replicas) }}</small
          ></strong
        >
      </div>
      <div class="replication-stat">
        <span>待追平 Offset</span
        ><strong>{{ number(stats?.replication_pending_offsets) }}</strong>
      </div>
    </section>
    <div class="topology-details">
      <section class="panel detail-panel">
        <h3><GitBranch :size="17" />复制链路</h3>
        <div class="replication-link">
          <span class="topology-endpoint"
            ><Server :size="18" /><strong>{{
              isReplica
                ? topology?.replication.master_node || "Upstream"
                : active?.name || "Primary"
            }}</strong
            ><small>Primary</small></span
          ><ArrowRight :size="24" class="muted" /><span
            class="topology-endpoint replica"
            ><Server :size="18" /><strong>{{
              isReplica
                ? active?.name
                : `${number(stats?.replication_configured_replicas)} 个副本`
            }}</strong
            ><small>Replica</small></span
          >
        </div>
        <div class="info-line">
          <span>Replication ID</span
          ><code>{{ topology?.replication.master_replid || "—" }}</code>
        </div>
        <div class="info-line">
          <span>复制方式</span><span>异步复制 · replid + offset</span>
        </div>
      </section>
      <section class="panel detail-panel">
        <h3><Crown :size="17" />故障转移</h3>
        <dl>
          <div>
            <dt>自动选主</dt>
            <dd>
              {{
                stats?.failover_enabled === undefined
                  ? "—"
                  : stats.failover_enabled
                    ? "已开启"
                    : "未开启"
              }}
            </dd>
          </div>
          <div>
            <dt>当前 Epoch</dt>
            <dd>{{ number(stats?.failover_current_epoch) }}</dd>
          </div>
          <div>
            <dt>可达节点 / Quorum</dt>
            <dd>
              {{ number(stats?.failover_reachable_nodes) }} /
              {{ number(stats?.failover_quorum) }}
            </dd>
          </div>
          <div>
            <dt>写入权限</dt>
            <dd>
              {{
                stats?.failover_enabled === undefined
                  ? "—"
                  : !stats.failover_enabled
                    ? "未启用选主隔离"
                    : stats.failover_writes_allowed
                      ? "允许"
                      : "已隔离"
              }}
            </dd>
          </div>
        </dl>
        <NPopconfirm
          v-if="
            connected &&
            isReplica &&
            topology?.clusterEnabled &&
            stats?.failover_enabled === false
          "
          @positive-click="
            state.page = 'workbench';
            execute('CLUSTER FAILOVER TAKEOVER');
          "
          ><template #trigger
            ><NButton size="small" secondary type="warning"
              >手动接管主节点</NButton
            ></template
          >将当前副本提升为主节点并接管原主节点的槽位？</NPopconfirm
        >
      </section>
    </div>
    <section class="panel nodes-panel">
      <header class="panel-toolbar">
        <div class="section-heading">
          <Network :size="17" /><span>集群节点</span
          ><span class="count-badge">{{ topology?.nodes.length ?? 0 }}</span>
        </div>
        <div class="toolbar-actions">
          <NButton
            size="small"
            :disabled="
              !connected || !topology?.clusterEnabled || !topology?.nodes.length
            "
            @click="
              operation = 'migrate';
              form.target = '';
            "
            ><template #icon><MoveRight :size="15" /></template
            >迁移槽位</NButton
          ><NButton
            size="small"
            :disabled="
              !connected || !topology?.clusterEnabled || !topology?.nodes.length
            "
            @click="
              operation = 'meet';
              form.target = '';
            "
            ><template #icon><Plus :size="15" /></template>加入节点</NButton
          >
        </div>
      </header>
      <div v-if="!topology?.nodes.length" class="empty-state compact-empty">
        <Layers3 :size="30" />
        <h3>当前没有集群拓扑</h3>
        <p>
          {{
            topology?.clusterError ||
            "连接后获取节点信息；单节点仍可使用复制功能。"
          }}
        </p>
      </div>
      <div v-else class="table-scroll">
        <table class="nodes-table">
          <thead>
            <tr>
              <th>节点地址</th>
              <th>角色</th>
              <th>槽位范围</th>
              <th>健康状态</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="node in topology.nodes" :key="node.id">
              <td>
                <Server :size="16" /><code>{{ node.address }}</code
                ><span v-if="node.current" class="subtle-tag">当前</span>
              </td>
              <td>
                <span
                  class="role-tag"
                  :class="node.role === 'replica' ? 'purple' : ''"
                  >{{ node.role === "replica" ? "Replica" : "Primary" }}</span
                >
              </td>
              <td class="slot-ranges">{{ node.slots || "—" }}</td>
              <td>
                <span
                  class="status-dot"
                  :class="
                    node.state === 'healthy'
                      ? 'online'
                      : node.state === 'suspect'
                        ? 'pending'
                        : 'failed'
                  "
                />{{
                  { healthy: "正常", suspect: "疑似故障", failed: "故障" }[
                    node.state
                  ]
                }}
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>
    <p class="page-footnote">
      集群为实验性功能。节点状态和槽位信息来自当前连接的 CLUSTER NODES。
    </p>
    <NModal :show="!!operation" @update:show="!$event && (operation = null)"
      ><div
        class="dialog small-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="topology-operation-title"
      >
        <header class="dialog-header">
          <div class="dialog-icon"><Link2 :size="22" /></div>
          <button
            class="icon-button"
            aria-label="关闭"
            @click="operation = null"
          >
            <X :size="18" />
          </button>
        </header>
        <h2 id="topology-operation-title">
          {{ operation === "meet" ? "加入集群节点" : "迁移槽位" }}
        </h2>
        <p class="muted">
          操作将在当前连接 {{ active?.host }}:{{ active?.port }} 执行。
        </p>
        <NForm
          label-placement="top"
          :show-feedback="false"
          class="connection-form"
          ><NFormItem v-if="operation === 'migrate'" label="槽位"
            ><NInputNumber
              v-model:value="form.slot"
              :min="0"
              :max="16383" /></NFormItem
          ><NFormItem label="目标节点"
            ><NInput
              v-model:value="form.target"
              placeholder="127.0.0.1:6367" /></NFormItem
        ></NForm>
        <footer class="dialog-actions">
          <NButton @click="operation = null">取消</NButton
          ><NButton type="primary" @click="submit">执行并查看结果</NButton>
        </footer>
      </div></NModal
    >
  </div>
</template>
