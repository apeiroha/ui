#!/usr/bin/env node
// Run the ui benchmark binaries and emit a single JSON run record.
//
// Each binary is executed `--iters` times and the minimum ns/op per metric
// is kept, since the minimum is the least noisy estimator on a shared runner.

import { execFileSync } from "node:child_process";
import { writeFileSync } from "node:fs";
import { cpus } from "node:os";
import { resolve } from "node:path";
import process from "node:process";

const GROUPS = [
  ["bench_ui", "bench_ui"],
  ["bench_ui_io", "bench_ui_io"],
];
const LINE = /^\s+(?<name>.*?\S)\s+(?<value>\d+(?:\.\d+)?)\s+ns\/op\b/;

function parseArgs(argv) {
  const opts = { binDir: "build", iters: 3, attempts: 8, out: "bench-output.json" };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--bin-dir") opts.binDir = argv[++i];
    else if (argv[i] === "--iters") opts.iters = Number(argv[++i]);
    else if (argv[i] === "--attempts") opts.attempts = Number(argv[++i]);
    else if (argv[i] === "--out") opts.out = argv[++i];
  }
  return opts;
}

function parseMetrics(text) {
  const metrics = {};
  for (const line of text.split("\n")) {
    const m = LINE.exec(line);
    if (m) metrics[m.groups.name] = Number(m.groups.value);
  }
  return metrics;
}

function collect(binDir, binary, iters, attempts) {
  const path = resolve(binDir, binary);
  const best = {};
  let ok = 0;
  for (let tries = 1; tries <= attempts && ok < iters; tries++) {
    let stdout;
    try {
      stdout = execFileSync(path, { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
    } catch (err) {
      // The runtime has a known intermittent scheduler race; a crashed run
      // yields no usable record, so retry rather than fail the whole job.
      console.warn(`[warn] ${binary} attempt ${tries} failed (${err.status ?? err.message}); retrying`);
      continue;
    }
    const parsed = parseMetrics(stdout);
    if (Object.keys(parsed).length === 0) {
      console.warn(`[warn] ${binary} attempt ${tries} produced no metrics; retrying`);
      continue;
    }
    ok++;
    for (const [name, value] of Object.entries(parsed)) {
      if (!(name in best) || value < best[name]) best[name] = value;
    }
  }
  if (ok === 0) throw new Error(`${binary}: no successful run in ${attempts} attempt(s)`);
  if (ok < iters) console.warn(`[warn] ${binary}: only ${ok}/${iters} successful run(s)`);
  return best;
}

function nowIso() {
  return new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const metrics = {};
  for (const [group, binary] of GROUPS) metrics[group] = collect(opts.binDir, binary, opts.iters, opts.attempts);

  const env = process.env;
  const server = env.GITHUB_SERVER_URL || "https://github.com";
  const repo = env.GITHUB_REPOSITORY || "apeiroha/ui";
  const sha = env.GITHUB_SHA || "";
  const runId = env.GITHUB_RUN_ID || "";
  const date = nowIso();

  const record = {
    id: runId ? `gh-${runId}` : `local-${date}`,
    commit: sha,
    short: sha.slice(0, 8),
    commit_url: sha ? `${server}/${repo}/commit/${sha}` : "",
    ref: env.GITHUB_REF_NAME || "local",
    date,
    run_url: runId ? `${server}/${repo}/actions/runs/${runId}` : "",
    env: {
      nvcpus: cpus().length,
      toolchain: env.TOOLCHAIN || "clang",
      build: env.BUILD || "release",
      iters: opts.iters,
    },
    metrics,
  };

  writeFileSync(opts.out, JSON.stringify(record, null, 2) + "\n");
  const summary = Object.entries(metrics).map(([g, m]) => `${g}=${Object.keys(m).length}`).join(", ");
  console.log(`wrote ${opts.out} (${summary})`);
}

main();
