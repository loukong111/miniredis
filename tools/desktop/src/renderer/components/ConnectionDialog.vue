<script setup lang="ts">
import { reactive, ref, watch } from "vue";
import {
  NButton,
  NCheckbox,
  NForm,
  NFormItem,
  NInput,
  NInputNumber,
  NModal,
  NSwitch,
} from "naive-ui";
import { ArrowUpRight, Database, ShieldCheck, X } from "@lucide/vue";
import type { ConnectionProfile } from "../../shared/contracts";
import { connectProfile, errorText, notice, state } from "../store";

const props = defineProps<{
  show: boolean;
  profile: ConnectionProfile | null;
}>();
const emit = defineEmits<{ "update:show": [value: boolean] }>();
const saving = ref(false);
const form = reactive({
  name: "",
  host: "127.0.0.1",
  port: 6366,
  statsPort: 8080,
  username: "",
  password: "",
  color: "#3b82f6",
  followRedirects: true,
  forgetPassword: false,
});
watch(
  () => props.show,
  (show) => {
    form.password = "";
    if (show)
      Object.assign(
        form,
        {
          name: "",
          host: "127.0.0.1",
          port: 6366,
          statsPort: 8080,
          username: "",
          color: "#3b82f6",
          followRedirects: true,
          forgetPassword: false,
        },
        props.profile ?? {},
        { password: "" },
      );
  },
);

async function save(connect: boolean): Promise<void> {
  if (saving.value) return;
  saving.value = true;
  try {
    const profile = await window.desktop.saveConnection({
      ...form,
      id: props.profile?.id,
    });
    const index = state.profiles.findIndex((item) => item.id === profile.id);
    if (index >= 0) state.profiles[index] = profile;
    else state.profiles.push(profile);
    state.selectedId = profile.id;
    emit("update:show", false);
    form.password = "";
    if (connect) await connectProfile(profile.id);
    else notice("连接已保存", "success");
  } catch (error) {
    notice(errorText(error), "error");
  } finally {
    saving.value = false;
  }
}
</script>

<template>
  <NModal
    :show="show"
    :mask-closable="!saving"
    @update:show="emit('update:show', $event)"
  >
    <div
      class="dialog connection-dialog"
      role="dialog"
      aria-modal="true"
      aria-labelledby="connection-dialog-title"
    >
      <header class="dialog-header">
        <div class="dialog-icon"><Database :size="22" /></div>
        <button
          class="icon-button"
          aria-label="关闭"
          :disabled="saving"
          @click="emit('update:show', false)"
        >
          <X :size="19" />
        </button>
      </header>
      <h2 id="connection-dialog-title">
        {{ profile ? "编辑连接" : "添加一个连接" }}
      </h2>
      <p class="muted">连接到你的 MiniRedis 实例，开始探索与调试。</p>
      <NForm
        label-placement="top"
        :show-feedback="false"
        class="connection-form"
        @submit.prevent="save(true)"
      >
        <NFormItem label="连接名称"
          ><NInput
            v-model:value="form.name"
            placeholder="例如：本地开发 / 测试集群"
            autofocus
            data-testid="connection-name"
            maxlength="80"
        /></NFormItem>
        <div class="form-row host-row">
          <NFormItem label="主机地址"
            ><NInput
              v-model:value="form.host"
              placeholder="127.0.0.1"
              data-testid="connection-host" /></NFormItem
          ><NFormItem label="RESP 端口"
            ><NInputNumber
              v-model:value="form.port"
              :min="1"
              :max="65535"
              :show-button="false"
              data-testid="connection-port"
          /></NFormItem>
        </div>
        <div class="form-row">
          <NFormItem label="监控端口"
            ><NInputNumber
              v-model:value="form.statsPort"
              :min="1"
              :max="65535"
              :show-button="false"
              data-testid="connection-stats" /></NFormItem
          ><NFormItem label="ACL 用户名（可选）"
            ><NInput
              v-model:value="form.username"
              placeholder="留空使用默认用户"
          /></NFormItem>
        </div>
        <NFormItem label="密码"
          ><NInput
            v-model:value="form.password"
            type="password"
            show-password-on="click"
            :placeholder="
              profile?.hasPassword
                ? '已保存密码，留空保持不变'
                : '未启用认证时留空'
            "
            data-testid="connection-password"
        /></NFormItem>
        <NCheckbox
          v-if="profile?.hasPassword"
          v-model:checked="form.forgetPassword"
          >移除已保存密码</NCheckbox
        >
        <div class="form-option">
          <span
            >自动跟随集群重定向
            <span class="muted small">MOVED / ASK</span></span
          ><NSwitch v-model:value="form.followRedirects" size="small" />
        </div>
        <div class="form-option">
          <span>连接标识</span>
          <div class="color-options">
            <button
              v-for="color in [
                '#3b82f6',
                '#10b981',
                '#8b5cf6',
                '#f59e0b',
                '#f43f5e',
              ]"
              :key="color"
              type="button"
              :style="{ background: color }"
              :class="{ selected: form.color === color }"
              :aria-label="color"
              @click="form.color = color"
            />
          </div>
        </div>
        <p class="storage-note">
          <ShieldCheck :size="15" />{{
            state.workspace?.passwordStorage === "encrypted"
              ? "密码使用系统密钥环加密保存。"
              : "系统密钥环不可用，密码仅保留在本次会话。"
          }}
        </p>
        <footer class="dialog-actions">
          <NButton :disabled="saving" @click="save(false)">仅保存</NButton
          ><NButton
            type="primary"
            :loading="saving"
            data-testid="connection-save"
            @click="save(true)"
            >保存并连接<template #icon><ArrowUpRight :size="16" /></template
          ></NButton>
        </footer>
      </NForm>
    </div>
  </NModal>
</template>
