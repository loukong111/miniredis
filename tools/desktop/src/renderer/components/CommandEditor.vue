<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from "vue";
import * as monaco from "monaco-editor/editor/editor.api.js";
import editorWorker from "monaco-editor/editor/editor.worker.js?worker";
import { isDark } from "../store";

const props = defineProps<{
  modelValue: string;
  disabled?: boolean;
  connectionId?: string | null;
}>();
const emit = defineEmits<{
  "update:modelValue": [value: string];
  run: [script: string];
}>();
const element = ref<HTMLElement>();
let editor: monaco.editor.IStandaloneCodeEditor | undefined;
let completion: monaco.IDisposable | undefined;
let updating = false;

const commands: Record<string, string> = {
  PING: "PING [message]",
  SET: "SET key value [EX seconds]",
  GET: "GET key",
  SETNX: "SETNX key value",
  GETDEL: "GETDEL key",
  GETEX: "GETEX key EX seconds",
  MGET: "MGET key [key ...]",
  APPEND: "APPEND key value",
  STRLEN: "STRLEN key",
  TYPE: "TYPE key",
  INCR: "INCR key",
  DECR: "DECR key",
  INCRBY: "INCRBY key increment",
  DECRBY: "DECRBY key decrement",
  DEL: "DEL key [key ...]",
  EXISTS: "EXISTS key [key ...]",
  EXPIRE: "EXPIRE key seconds",
  PEXPIRE: "PEXPIRE key milliseconds",
  TTL: "TTL key",
  PTTL: "PTTL key",
  PERSIST: "PERSIST key",
  INFO: "INFO [server|clients|memory|stats|persistence|replication|cluster]",
  SLOWLOG: "SLOWLOG GET [count] / LEN / RESET",
  COMMAND: "COMMAND [COUNT|INFO command]",
  AUTH: "AUTH [username] password",
  ACL: "ACL WHOAMI / LIST / GETUSER username",
  BGREWRITEAOF: "BGREWRITEAOF",
  CLUSTER: "CLUSTER INFO / NODES / SLOTS / MIGRATE slot target",
  ASKING: "ASKING",
};

function selectedScript(): string {
  if (!editor) return "";
  const selection = editor.getSelection();
  if (selection && !selection.isEmpty())
    return editor.getModel()?.getValueInRange(selection) ?? "";
  const position = editor.getPosition();
  return position
    ? (editor.getModel()?.getLineContent(position.lineNumber) ?? "")
    : "";
}

defineExpose({
  runSelected: () => emit("run", selectedScript()),
  focus: () => editor?.focus(),
});

onMounted(() => {
  (self as unknown as { MonacoEnvironment: unknown }).MonacoEnvironment = {
    getWorker: () => new editorWorker(),
  };
  if (
    !monaco.languages
      .getLanguages()
      .some((language) => language.id === "miniredis")
  ) {
    monaco.languages.register({ id: "miniredis" });
    monaco.languages.setMonarchTokensProvider("miniredis", {
      ignoreCase: true,
      keywords: Object.keys(commands),
      tokenizer: {
        root: [
          [/#.*$/, "comment"],
          [
            /\b[A-Z]+\b/,
            { cases: { "@keywords": "keyword", "@default": "identifier" } },
          ],
          [/"([^"\\]|\\.)*"|'([^'\\]|\\.)*'/, "string"],
          [/\b\d+\b/, "number"],
        ],
      },
    });
    monaco.editor.defineTheme("miniredis-light", {
      base: "vs",
      inherit: true,
      rules: [
        { token: "keyword", foreground: "3468D5", fontStyle: "bold" },
        { token: "string", foreground: "248A68" },
        { token: "comment", foreground: "94A0B2" },
      ],
      colors: {
        "editor.background": "#FFFFFF",
        "editorLineNumber.foreground": "#BCC4D0",
        "editor.lineHighlightBackground": "#F6F8FC",
        "editor.selectionBackground": "#DCE9FE",
      },
    });
    monaco.editor.defineTheme("miniredis-dark", {
      base: "vs-dark",
      inherit: true,
      rules: [{ token: "keyword", foreground: "88B4FF", fontStyle: "bold" }],
      colors: {
        "editor.background": "#171D28",
        "editor.lineHighlightBackground": "#1C2432",
      },
    });
  }
  completion = monaco.languages.registerCompletionItemProvider("miniredis", {
    provideCompletionItems(model, position) {
      const word = model.getWordUntilPosition(position);
      return {
        suggestions: Object.entries(commands).map(([label, detail]) => ({
          label,
          detail,
          kind: monaco.languages.CompletionItemKind.Function,
          insertText: label,
          range: new monaco.Range(
            position.lineNumber,
            word.startColumn,
            position.lineNumber,
            word.endColumn,
          ),
        })),
      };
    },
  });
  editor = monaco.editor.create(element.value!, {
    value: props.modelValue,
    language: "miniredis",
    theme: isDark.value ? "miniredis-dark" : "miniredis-light",
    automaticLayout: true,
    minimap: { enabled: false },
    fontSize: 14,
    lineHeight: 26,
    fontFamily: '"JetBrains Mono", "SFMono-Regular", Consolas, monospace',
    padding: { top: 20, bottom: 16 },
    scrollBeyondLastLine: false,
    renderLineHighlight: "line",
    roundedSelection: true,
    lineNumbersMinChars: 3,
    overviewRulerLanes: 0,
    hideCursorInOverviewRuler: true,
    glyphMargin: false,
    folding: false,
    wordWrap: "on",
    tabSize: 2,
    readOnly: props.disabled,
    contextmenu: true,
    fixedOverflowWidgets: true,
  });
  editor.onDidChangeModelContent(() => {
    if (!updating) emit("update:modelValue", editor?.getValue() ?? "");
  });
  editor.addAction({
    id: "miniredis.run",
    label: "执行选中 / 当前行",
    keybindings: [monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter],
    run: () => emit("run", selectedScript()),
  });
  editor.addAction({
    id: "miniredis.runAll",
    label: "执行全部",
    keybindings: [
      monaco.KeyMod.CtrlCmd | monaco.KeyMod.Shift | monaco.KeyCode.Enter,
    ],
    run: () => emit("run", editor?.getValue() ?? ""),
  });
});

watch(
  () => props.modelValue,
  (value) => {
    if (editor && editor.getValue() !== value) {
      updating = true;
      editor.setValue(value);
      updating = false;
    }
  },
);
watch(
  () => props.disabled,
  (value) => editor?.updateOptions({ readOnly: value }),
);
watch(isDark, (value) =>
  monaco.editor.setTheme(value ? "miniredis-dark" : "miniredis-light"),
);
watch(
  () => props.connectionId,
  () => editor?.setScrollTop(0),
);
onBeforeUnmount(() => {
  completion?.dispose();
  editor?.getModel()?.dispose();
  editor?.dispose();
});
</script>

<template>
  <div
    ref="element"
    class="command-editor"
    data-testid="command-editor"
    aria-label="Redis 命令编辑器"
  />
</template>
