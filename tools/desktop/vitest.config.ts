import { defineConfig } from "vitest/config";
export default defineConfig({
  test: {
    include: ["tests/**/*.test.ts"],
    testTimeout: 15000,
    coverage: {
      provider: "v8",
      include: [
        "src/main/{protocol,redis,workspace,processes}.ts",
        "src/shared/commands.ts",
        "src/renderer/store.ts",
      ],
      reporter: ["text", "json-summary", "html"],
      reportsDirectory: "../../build-review/desktop-coverage",
      thresholds: { statements: 88, branches: 80, functions: 85, lines: 90 },
    },
  },
});
