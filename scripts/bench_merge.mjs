#!/usr/bin/env node
// Append a benchmark run record to the historical dataset.
//
// Usage: bench_merge.mjs RUN.json HISTORY.json
//
// The history file is upserted by run id, sorted by date and capped at
// MAX_RUNS so the published page stays small.

import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";
import process from "node:process";

const MAX_RUNS = 300;

function load(path) {
  if (!existsSync(path)) return { runs: [] };
  return JSON.parse(readFileSync(path, "utf8"));
}

function main() {
  const [runPath, histPath] = process.argv.slice(2);
  if (!runPath || !histPath) {
    console.error("usage: bench_merge.mjs RUN.json HISTORY.json");
    process.exit(2);
  }

  const run = JSON.parse(readFileSync(runPath, "utf8"));
  const history = load(histPath);
  const runs = (history.runs || []).filter((r) => r.id !== run.id);
  runs.push(run);
  runs.sort((a, b) => String(a.date).localeCompare(String(b.date)));
  history.runs = runs.slice(-MAX_RUNS);
  history.generated_at = run.date || "";

  const dir = dirname(histPath);
  if (dir && dir !== ".") mkdirSync(dir, { recursive: true });
  writeFileSync(histPath, JSON.stringify(history, null, 2) + "\n");
  console.log(`history now holds ${history.runs.length} run(s)`);
}

main();
