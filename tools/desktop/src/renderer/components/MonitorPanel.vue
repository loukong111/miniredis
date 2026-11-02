<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { NButton, NDrawer, NDrawerContent } from "naive-ui";
import {
  Activity,
  ArrowUpRight,
  CheckCircle2,
  CircleHelp,
  Clock3,
  Code2,
  Database,
  Gauge,
  HardDrive,
  MemoryStick,
  Radio,
  RefreshCw,
  ShieldCheck,
} from "@lucide/vue";
import Sparkline from "./Sparkline.vue";
import {
  active,
  bytes,
  connected,
  errorText,
  notice,
  number,
  samples,
  snapshot,
  state,
  stats,
} from "../store";

const latest = computed(() => samples.value.at(-1));
const metricsOpen = ref(false);
const metrics = ref("");
const metricsLoading = ref(false);
const metricsError = ref("");
let metricsGeneration = 0;
watch([() => state.selectedId, connected, () => state.page], () => {
  metricsGeneration++;
  metricsOpen.value = false;
  metricsLoading.value = false;
  metrics.value = "";
  metricsError.value = "";
});
const cards = computed(() => [
  {
    label: "每秒命令",
    value: number(latest.value?.qps, 1),
    unit: "cmd/s",
    icon: Activity,
    color: "#4d81f6",
    values: samples.value.map((item) => item.qps),
  },
  {
    label: "存活 Key",
    value: number(stats.value?.key_count),
    unit: "keys",
    icon: Database,
    color: "#8b5cf6",
    values: [],
  },
  {
    label: "数据内存",
    value: bytes(stats.value?.used_memory_bytes),
    unit: "",
    icon: MemoryStick,
    color: "#13a98c",
    values: samples.value.map((item) => item.memory),
  },
  {
    label: "GET 命中率",
    value:
      stats.value?.hit_rate === undefined
        ? "—"
        : number(Number(stats.value.hit_rate) * 100, 1),
    unit: "%",
    icon: Gauge,
    color: "#e9a03c",
    values: [],
  },
]);
function date(value: unknown): string {
  return Number(value) > 0
    ? new Date(Number(value)).toLocaleString("zh-CN", { hour12: false })
    : "暂无记录";
}
async function showMetrics(): Promise<void> {
  const id = active.value?.id;
  if (!id || !connected.value) return;
  const generation = ++metricsGeneration;
  metricsOpen.value = true;
  metricsLoading.value = true;
  metricsError.value = "";
  try {
    const value = await window.desktop.metrics(id);
    if (generation === metricsGeneration) metrics.value = value;
  } catch (error) {
    if (generation !== metricsGeneration) return;
    metricsError.value = errorText(error);
    notice(metricsError.value, "error");
  } finally {
    if (generation === metricsGeneration) metricsLoading.value = false;
  }
}
</script>

<template>
  <div class="page-scroll monitoring-page">
    <header class="page-heading">
      <div>
        <div class="eyebrow">OBSERVABILITY</div>
        <h1>运行监控</h1>
        <p>了解实例的流量、内存与持久化状态。</p>
      </div>
      <NButton size="small" :disabled="!connected" @click="showMetrics"
        ><template #icon><Code2 :size="15" /></template>Prometheus<ArrowUpRight
          :size="13"
          class="button-tail"
      /></NButton>
    </header>
    <div v-if="snapshot?.error || !connected" class="inline-alert">
      <CircleHelp :size="17" /><span>{{
        !connected ? "连接实例后显示实时指标。" : snapshot?.error
      }}</span>
    </div>
    <div class="metric-cards">
      <section
        v-for="card in cards"
        :key="card.label"
        class="metric-card panel"
      >
        <div class="metric-title">
          <span>{{ card.label }}</span
          ><component
            :is="card.icon"
            :size="18"
            :style="{ color: card.color }"
          />
        </div>
        <div class="metric-value">
          {{ card.value }}<span>{{ card.unit }}</span>
        </div>
        <Sparkline
          v-if="card.values.length > 1"
          :values="card.values"
          :height="40"
          :color="card.color"
          :label="card.label"
        />
        <div v-else class="metric-caption">
          {{ snapshot?.stats ? "来自当前实例" : "等待真实数据" }}
        </div>
      </section>
    </div>
    <div class="chart-panels">
      <section class="panel chart-panel">
        <header>
          <div>
            <h3><Activity :size="16" />命令吞吐</h3>
            <p>根据命令总数增量计算</p>
          </div>
          <span class="chart-current"
            >{{ number(latest?.qps, 1) }} <small>cmd/s</small></span
          >
        </header>
        <Sparkline
          :values="samples.map((item) => item.qps)"
          label="每秒命令趋势"
          :height="170"
        />
        <footer>
          <span>最近 {{ samples.length }} 次采样</span
          ><span class="chart-legend"><i />Commands / sec</span>
        </footer>
      </section>
      <section class="panel chart-panel">
        <header>
          <div>
            <h3><Clock3 :size="16" />平均命令延迟</h3>
            <p>实例累计平均值的变化</p>
          </div>
          <span class="chart-current"
            >{{ number(stats?.avg_command_latency_us, 1) }}
            <small>µs</small></span
          >
        </header>
        <Sparkline
          :values="samples.map((item) => item.latency)"
          label="平均延迟趋势"
          :height="170"
          color="#13a98c"
        />
        <footer>
          <span>最近 {{ samples.length }} 次采样</span
          ><span class="chart-legend green"><i />Latency</span>
        </footer>
      </section>
    </div>
    <div class="detail-panels">
      <section class="panel detail-panel">
        <h3><Radio :size="17" />连接与资源</h3>
        <dl>
          <div>
            <dt>当前连接</dt>
            <dd>{{ number(stats?.connected_clients) }}</dd>
          </div>
          <div>
            <dt>拒绝连接</dt>
            <dd>{{ number(stats?.rejected_connections) }}</dd>
          </div>
          <div>
            <dt>内存额度</dt>
            <dd>
              {{
                stats?.maxmemory_bytes === 0
                  ? "不限制"
                  : bytes(stats?.maxmemory_bytes)
              }}
            </dd>
          </div>
          <div>
            <dt>已淘汰 Key</dt>
            <dd>{{ number(stats?.evicted_keys) }}</dd>
          </div>
          <div>
            <dt>IO 线程 / 分片</dt>
            <dd>
              {{ number(stats?.io_threads) }} /
              {{ number(stats?.cache_shards) }}
            </dd>
          </div>
        </dl>
      </section>
      <section class="panel detail-panel">
        <h3><HardDrive :size="17" />持久化</h3>
        <dl>
          <div>
            <dt>快照任务</dt>
            <dd>
              {{
                stats?.snapshot_running === undefined
                  ? "—"
                  : stats.snapshot_running
                    ? "运行中"
                    : "空闲"
              }}
            </dd>
          </div>
          <div>
            <dt>最近快照</dt>
            <dd>{{ date(stats?.snapshot_last_success_unix_ms) }}</dd>
          </div>
          <div>
            <dt>AOF 重写</dt>
            <dd>
              {{
                stats?.aof_rewrite_running === undefined
                  ? "—"
                  : stats.aof_rewrite_running
                    ? "运行中"
                    : String(stats.aof_rewrite_last_status || "暂无记录")
              }}
            </dd>
          </div>
          <div>
            <dt>重写缓冲</dt>
            <dd>{{ bytes(stats?.aof_rewrite_buffer_bytes) }}</dd>
          </div>
          <div>
            <dt>快照 / 重写失败</dt>
            <dd>
              {{ number(stats?.snapshot_failures) }} /
              {{ number(stats?.aof_rewrite_failures) }}
            </dd>
          </div>
        </dl>
      </section>
      <section class="panel detail-panel">
        <h3><ShieldCheck :size="17" />服务健康</h3>
        <dl>
          <div>
            <dt>存活检查</dt>
            <dd>
              <span
                class="health-label"
                :class="snapshot?.healthy ? 'success-text' : ''"
                ><CheckCircle2 v-if="snapshot?.healthy" :size="13" />{{
                  snapshot?.healthy === undefined
                    ? "—"
                    : snapshot.healthy
                      ? "正常"
                      : "异常"
                }}</span
              >
            </dd>
          </div>
          <div>
            <dt>就绪检查</dt>
            <dd>
              {{
                snapshot?.ready === undefined
                  ? "—"
                  : snapshot.ready
                    ? "已就绪"
                    : "未就绪"
              }}
            </dd>
          </div>
          <div>
            <dt>副本在线</dt>
            <dd>
              {{ number(stats?.replication_connected_replicas) }} /
              {{ number(stats?.replication_configured_replicas) }}
            </dd>
          </div>
          <div>
            <dt>待追平 offset</dt>
            <dd>{{ number(stats?.replication_pending_offsets) }}</dd>
          </div>
          <div>
            <dt>慢日志条数</dt>
            <dd>{{ number(stats?.slowlog_len) }}</dd>
          </div>
        </dl>
      </section>
    </div>
    <footer class="monitoring-footer">
      <span
        ><span
          class="status-dot"
          :class="connected && snapshot?.stats ? 'online' : 'offline'"
        />{{
          snapshot
            ? `最近更新 ${new Date(snapshot.timestamp).toLocaleTimeString("zh-CN", { hour12: false })}`
            : "尚未收到监控数据"
        }}</span
      ><span>内存为 Key / Value 字节统计，不等同于进程 RSS</span>
    </footer>
    <NDrawer v-model:show="metricsOpen" :width="640"
      ><NDrawerContent title="Prometheus 指标" closable
        ><div v-if="metricsLoading" class="empty-state">
          <RefreshCw :size="22" class="spinning" />
          <p>读取指标中…</p>
        </div>
        <p v-else-if="metricsError" class="danger-text">{{ metricsError }}</p>
        <pre v-else class="raw-output">{{ metrics }}</pre>
      </NDrawerContent></NDrawer
    >
  </div>
</template>
