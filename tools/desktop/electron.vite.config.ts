import { defineConfig } from "electron-vite";
import vue from "@vitejs/plugin-vue";

export default defineConfig({
  main: {},
  preload: {
    build: {
      rollupOptions: { output: { format: "cjs", entryFileNames: "index.cjs" } },
    },
  },
  renderer: {
    plugins: [vue()],
    build: { minify: true, chunkSizeWarningLimit: 4000 },
  },
});
