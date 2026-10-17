import type { BenchData } from "./types";

// Relative to the page, so it resolves under the deploy base (/ui/).
export async function loadBench(): Promise<BenchData> {
  const res = await fetch("data/bench.json", { cache: "no-store" });
  if (!res.ok) throw new Error("HTTP " + res.status);
  return res.json() as Promise<BenchData>;
}
