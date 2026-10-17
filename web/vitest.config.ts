import { defineConfig } from "vitest/config";

// The dashboard's selection/diff math lives in src/metrics.ts with no DOM
// dependency, so the tests run in plain node.
export default defineConfig({
  test: {
    environment: "node",
    include: ["src/**/*.test.ts"],
  },
});
