<script setup lang="ts">
import { computed, nextTick, ref, watch } from "vue";
import { NButton, NDropdown, NTooltip } from "naive-ui";
import {
  ArrowDownToLine,
  BookOpen,
  Check,
  CheckCheck,
  ChevronDown,
  Clock3,
  Code2,
  Copy,
  History,
  Play,
  Square,
  Terminal,
  Trash2,
  XCircle,
} from "@lucide/vue";
import type { CommandResult } from "../../shared/contracts";
import { textResponse } from "../../shared/commands";
import CommandEditor from "./CommandEditor.vue";
import {
  active,
  connected,
  errorText,
  execute,
  notice,
  results,
  running,
  script,
  state,
} from "../store";

const editor = ref<InstanceType<typeof CommandEditor>>();
const responseList = ref<HTMLElement>();
const helpOpen = ref(false);
const expanded = ref(new Set<string>());
const formatted = new WeakMap<CommandResult, string>();
function responseText(result: CommandResult): string {
  let value = formatted.get(result);
  if (value === undefined) {
    value = textResponse(result.response);
    formatted.set(result, value);
  }
  return value;
}
function preview(result: CommandResult): string {
  const limit = expanded.value.has(result.id) ? 200_000 : 20_000;
  const value = responseText(result);
  return value.length > limit
    ? value.slice(0, limit) + "\n… 响应较长，可复制或导出完整内容"
    : value;
}
watch(
  () => state.selectedId,
  () => expanded.value.clear(),
);
const history = computed(() =>
  (state.selectedId ? (state.history[state.selectedId] ?? []) : []).map(
    (text, index) => ({
      label:
        text.split("\n")[0].slice(0, 60) + (text.includes("\n") ? " …" : ""),
      key: String(index),
    }),
  ),
);
const templates = [
  {
    label: "基础读写与 TTL",
    key: "basic",
    text: 'SET demo:greeting "Hello MiniRedis" EX 120\nGET demo:greeting\nTTL demo:greeting',
  },
  {
    label: "计数器",
    key: "counter",
    text: "INCR demo:counter\nINCRBY demo:counter 10\nGET demo:counter",
  },
  { label: "持久化状态", key: "persistence", text: "INFO persistence" },
  { label: "复制状态", key: "replication", text: "INFO replication" },
  {
    label: "集群拓扑",
    key: "cluster",
    text: "CLUSTER INFO\nCLUSTER NODES\nCLUSTER SLOTS",
  },
  { label: "慢日志", key: "slowlog", text: "SLOWLOG LEN\nSLOWLOG GET 10" },
];
const supported = [
  ["读取", "GET key", "MGET key [key ...]", "STRLEN key", "TYPE key"],
  [
    "写入",
    "SET key value [EX seconds]",
    "SETNX key value",
    "APPEND key value",
    "INCR key / DECR key",
    "DEL key [key ...]",
  ],
  [
    "过期时间",
    "EXPIRE key seconds",
    "PEXPIRE key milliseconds",
    "TTL key / PTTL key",
    "PERSIST key",
  ],
  ["诊断", "INFO [section]", "SLOWLOG GET 10", "COMMAND INFO GET"],
];
const successCount = computed(
  () =>
    results.value.filter((result) => result.response.type !== "error").length,
);
const latest = computed(() => results.value.at(-1));

function insertTemplate(key: string): void {
  const template = templates.find((item) => item.key === key);
  if (template) {
    script.value = template.text;
    editor.value?.focus();
  }
}
function loadHistory(key: string): void {
  if (state.selectedId)
    script.value =
      state.history[state.selectedId]?.[Number(key)] ?? script.value;
}
async function copy(text: string): Promise<void> {
  try {
    await window.desktop.copyText(text);
    notice("已复制", "success");
  } catch {
    notice("无法访问剪贴板", "error");
  }
}
async function exportOutput(): Promise<void> {
  try {
    await window.desktop.exportText({
      filename: "miniredis-results.txt",
      content: results.value
        .map(
          (result) => `> ${result.command}\n${textResponse(result.response)}\n`,
        )
        .join("\n"),
    });
  } catch (error) {
    notice(errorText(error), "error");
  }
}
async function cancel(): Promise<void> {
  if (active.value)
    try {
      await window.desktop.cancel(active.value.id);
    } catch (error) {
      notice(errorText(error), "error");
    }
}
watch(
  () => results.value.length + ":" + latest.value?.id,
  async () => {
    await nextTick();
    responseList.value?.scrollTo({
      top: responseList.value.scrollHeight,
      behavior: "smooth",
    });
  },
);
</script>

<template>
  <div class="workbench-layout">
    <div class="workbench-main">
      <section class="editor-panel panel">
        <header class="panel-toolbar">
          <div class="section-heading">
            <Terminal :size="17" /><span>命令编辑器</span
            ><span class="subtle-tag">RESP 2</span>
          </div>
          <div class="toolbar-actions">
            <NDropdown
              :options="templates.map(({ label, key }) => ({ label, key }))"
              @select="insertTemplate"
              ><button class="text-button">
                <Code2 :size="15" />模板<ChevronDown :size="13" /></button
            ></NDropdown>
            <NDropdown
              :options="
                history.length
                  ? history
                  : [{ label: '还没有执行记录', key: 'empty', disabled: true }]
              "
              @select="loadHistory"
              ><button
                class="icon-button"
                aria-label="命令历史"
                title="命令历史"
              >
                <History :size="17" /></button
            ></NDropdown>
            <button
              class="icon-button"
              :class="{ active: helpOpen }"
              aria-label="命令参考"
              title="命令参考"
              @click="helpOpen = !helpOpen"
            >
              <BookOpen :size="17" />
            </button>
            <span class="toolbar-divider" />
            <NButton
              v-if="running"
              size="small"
              type="error"
              secondary
              @click="cancel"
              ><template #icon><Square :size="13" /></template>取消</NButton
            >
            <NButton
              v-else
              size="small"
              :disabled="!connected"
              @click="execute(script)"
              ><template #icon><CheckCheck :size="15" /></template
              >运行全部</NButton
            >
            <NButton
              type="primary"
              size="small"
              :disabled="!connected || running"
              data-testid="run-command"
              @click="editor?.runSelected()"
              ><template #icon><Play :size="14" fill="currentColor" /></template
              >运行</NButton
            >
          </div>
        </header>
        <CommandEditor
          ref="editor"
          v-model="script"
          :disabled="running"
          :connection-id="state.selectedId"
          @run="execute"
        />
        <footer class="editor-footer">
          <span
            ><span
              class="status-dot"
              :class="connected ? 'online' : 'offline'"
            />{{
              connected
                ? `${active?.host}:${active?.port}`
                : "连接实例后即可运行命令"
            }}</span
          ><span><kbd>Ctrl / ⌘</kbd> <kbd>↵</kbd> 执行当前行或选中内容</span>
        </footer>
      </section>

      <section class="results-panel panel">
        <header class="panel-toolbar">
          <div class="section-heading">
            <span class="result-indicator" /><span>执行结果</span
            ><span class="count-badge">{{ results.length }}</span
            ><span v-if="running" class="running-label"
              ><span class="spinner" />执行中</span
            >
          </div>
          <div class="toolbar-actions">
            <span v-if="results.length" class="muted small"
              >{{ successCount }} 成功 ·
              {{ results.length - successCount }} 错误</span
            ><button
              class="icon-button"
              :disabled="!results.length"
              aria-label="导出结果"
              title="导出结果"
              @click="exportOutput"
            >
              <ArrowDownToLine :size="16" /></button
            ><button
              class="icon-button"
              :disabled="!results.length || running"
              aria-label="清空结果"
              title="清空结果"
              @click="
                state.selectedId && (state.results[state.selectedId] = [])
              "
            >
              <Trash2 :size="16" />
            </button>
          </div>
        </header>
        <div ref="responseList" class="response-list" data-testid="results">
          <div v-if="!results.length" class="empty-results">
            <div class="empty-icon"><Terminal :size="25" /></div>
            <h3>让命令开始对话</h3>
            <p>输入一条命令，运行后在这里查看响应。</p>
            <div class="empty-command">PING <span>↵</span></div>
          </div>
          <article
            v-for="result in results"
            :key="result.id"
            class="response-card"
            :class="{ 'response-error': result.response.type === 'error' }"
            data-testid="result-card"
          >
            <header class="response-header">
              <Check
                v-if="result.response.type !== 'error'"
                :size="15"
                class="success-text"
              /><XCircle v-else :size="15" class="danger-text" /><code>{{
                result.command
              }}</code>
              <div class="response-meta">
                <span class="response-type">{{ result.response.type }}</span
                ><Clock3 :size="12" /><span
                  >{{ result.durationMs.toFixed(1) }} ms</span
                ><NTooltip
                  ><template #trigger
                    ><button
                      class="icon-button compact"
                      aria-label="复制响应"
                      @click="copy(responseText(result))"
                    >
                      <Copy :size="13" /></button></template
                  >复制响应</NTooltip
                >
              </div>
            </header>
            <div v-if="result.redirects.length" class="redirect-note">
              {{ result.redirects.join(" · ") }}
            </div>
            <pre :class="{ collapsed: !expanded.has(result.id) }">{{
              preview(result)
            }}</pre>
            <button
              v-if="responseText(result).length > 1500"
              class="text-button expand-result"
              @click="
                expanded.has(result.id)
                  ? expanded.delete(result.id)
                  : expanded.add(result.id)
              "
            >
              {{ expanded.has(result.id) ? "收起" : "展开响应"
              }}<ChevronDown :size="13" />
            </button>
          </article>
        </div>
        <footer class="results-footer">
          <span>{{
            results.length
              ? "保留最近 100 条响应 · 历史仅在本次会话保存"
              : "支持多行命令、选中执行与命令补全"
          }}</span
          ><span v-if="latest">{{
            new Date(latest.timestamp).toLocaleTimeString("zh-CN", {
              hour12: false,
            })
          }}</span>
        </footer>
      </section>
    </div>
    <aside v-if="helpOpen" class="command-help panel">
      <header class="panel-toolbar">
        <strong>命令参考</strong
        ><button
          class="icon-button compact"
          aria-label="收起命令参考"
          @click="helpOpen = false"
        >
          <ChevronDown :size="15" />
        </button>
      </header>
      <div
        v-for="[title, ...commands] in supported"
        :key="title"
        class="help-group"
      >
        <h4>{{ title }}</h4>
        <code v-for="command in commands" :key="command">{{ command }}</code>
      </div>
      <p class="help-footnote">
        仅展示 MiniRedis 已支持的命令。<br />输入引号可以包含空格，使用 \n
        表示换行。
      </p>
    </aside>
  </div>
</template>
