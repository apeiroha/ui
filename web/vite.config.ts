import { defineConfig } from "vite";
import solid from "@solidjs/vite-plugin";

// Published to https://apeiroha.github.io/ui/ (a project page), so assets
// are served under /ui/. The dashboard fetches data/bench.json relatively,
// which resolves under the same base.
export default defineConfig({
  base: "/ui/",
  plugins: [solid()],
  build: {
    outDir: "dist",
    emptyOutDir: true,
    rollupOptions: {
      input: {
        index: "index.html",
        compare: "compare.html",
      },
    },
  },
});
