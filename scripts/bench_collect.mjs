#!/usr/bin/env node
// Run the ui benchmark binaries and emit a single JSON run record.
//
// Each binary is executed `--iters` times and the minimum ns/op per metric
// is kept, since the minimum is the least noisy estimator on a shared runner.
//
// Same-machine A/B: pass --base-bin-dir to also measure a baseline build
// that was compiled from a different ref on this same runner. The two
// builds are then run *interleaved* (one round = head then base, order
// flipped every round) so machine drift over the job is shared by both
// sides instead of being attributed to whichever ran later. The baseline
// metrics are attached under `base` on the emitted record; the head stays
// at the top level so the tracking and per-ref compare pages keep working
// unchanged.

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
  const opts = {
    binDir: "build",
    baseBinDir: "",
    iters: 3,
    attempts: 8,
    out: "bench-output.json",
    ref: "",
    sha: "",
    baseRef: "",
    baseSha: "",
  };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--bin-dir") opts.binDir = argv[++i];
    else if (argv[i] === "--base-bin-dir") opts.baseBinDir = argv[++i];
    else if (argv[i] === "--iters") opts.iters = Number(argv[++i]);
    else if (argv[i] === "--attempts") opts.attempts = Number(argv[++i]);
    else if (argv[i] === "--out") opts.out = argv[++i];
    else if (argv[i] === "--ref") opts.ref = argv[++i];
    else if (argv[i] === "--sha") opts.sha = argv[++i];
    else if (argv[i] === "--base-ref") opts.baseRef = argv[++i];
    else if (argv[i] === "--base-sha") opts.baseSha = argv[++i];
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

// One invocation of one binary. Returns parsed metrics, or null on a
// non-usable run (crash or empty output) so the caller can retry. `side`
// only labels the warning; it distinguishes head from base in A/B mode.
function attemptRun(path, side, tries) {
  let stdout;
  try {
    stdout = execFileSync(path, {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
      timeout: 180000,
    });
  } catch (err) {
    // The runtime has a known intermittent scheduler race; a crashed run
    // yields no usable record, so retry rather than fail the whole job.
    const where = String(err.stdout || "").trim().split("\n").filter(Boolean).pop() || "";
    const how = err.signal ? `signal ${err.signal}` : `exit ${err.status}`;
    console.warn(`[warn] ${side} attempt ${tries} failed (${how})${where ? " last: " + where.trim() : ""}; retrying`);
    return null;
  }
  const parsed = parseMetrics(stdout);
  if (Object.keys(parsed).length === 0) {
    console.warn(`[warn] ${side} attempt ${tries} produced no metrics; retrying`);
    return null;
  }
  return parsed;
}

function mergeBest(best, parsed) {
  for (const [name, value] of Object.entries(parsed)) {
    if (!(name in best) || value < best[name]) best[name] = value;
  }
}

// Single ref: `iters` successful runs, min per metric.
function collectSingle(binDir, binary, iters, attempts) {
  const path = resolve(binDir, binary);
  const best = {};
  let ok = 0;
  for (let tries = 1; tries <= attempts && ok < iters; tries++) {
    const parsed = attemptRun(path, binary, tries);
    if (!parsed) continue;
    ok++;
    mergeBest(best, parsed);
  }
  if (ok === 0) throw new Error(`${binary}: no successful run in ${attempts} attempt(s)`);
  if (ok < iters) console.warn(`[warn] ${binary}: only ${ok}/${iters} successful run(s)`);
  return { metrics: best, samples: ok };
}

// Two refs, same machine: `iters` paired rounds. Both sides of a round must
// succeed or the whole round is retried, so every kept sample has a
// same-moment counterpart on the other side. Order flips each round to
// cancel any first-vs-second systematic bias.
function collectPair(headDir, baseDir, binary, iters, attempts) {
  const headPath = resolve(headDir, binary);
  const basePath = resolve(baseDir, binary);
  const head = {}, base = {};
  let ok = 0;
  for (let tries = 1; tries <= attempts && ok < iters; tries++) {
    const headFirst = ok % 2 === 0;
    const [p1, s1] = headFirst ? [headPath, "head"] : [basePath, "base"];
    const [p2, s2] = headFirst ? [basePath, "base"] : [headPath, "head"];
    const r1 = attemptRun(p1, `${binary}[${s1}]`, tries);
    const r2 = attemptRun(p2, `${binary}[${s2}]`, tries);
    if (!r1 || !r2) continue;
    ok++;
    mergeBest(s1 === "head" ? head : base, r1);
    mergeBest(s2 === "head" ? head : base, r2);
  }
  if (ok === 0) throw new Error(`${binary}: no successful paired run in ${attempts} attempt(s)`);
  if (ok < iters) console.warn(`[warn] ${binary}: only ${ok}/${iters} paired run(s)`);
  return { head: { metrics: head, samples: ok }, base: { metrics: base, samples: ok } };
}

function sideInfo(ref, sha, metrics, server, repo) {
  return {
    ref: ref || "local",
    commit: sha || "",
    short: sha ? sha.slice(0, 8) : "",
    commit_url: sha ? `${server}/${repo}/commit/${sha}` : "",
    metrics,
  };
}

function nowIso() {
  return new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const env = process.env;
  const server = env.GITHUB_SERVER_URL || "https://github.com";
  const repo = env.GITHUB_REPOSITORY || "apeiroha/ui";
  const runId = env.GITHUB_RUN_ID || "";
  const date = nowIso();

  // Explicit --ref/--sha win; the env fallbacks keep `make bench-json` and
  // a plain local run working. GITHUB_HEAD_REF is the branch on a PR
  // (GITHUB_REF_NAME there is "<pr>/merge", which is not a branch name).
  const ref = opts.ref || env.GITHUB_HEAD_REF || env.GITHUB_REF_NAME || "local";
  const sha = opts.sha || env.GITHUB_SHA || "";
  const ab = Boolean(opts.baseBinDir);

  const metrics = {};
  const baseMetrics = {};
  if (ab) {
    for (const [group, binary] of GROUPS) {
      const { head, base } = collectPair(opts.binDir, opts.baseBinDir, binary, opts.iters, opts.attempts);
      metrics[group] = head.metrics;
      baseMetrics[group] = base.metrics;
    }
  } else {
    for (const [group, binary] of GROUPS) {
      metrics[group] = collectSingle(opts.binDir, binary, opts.iters, opts.attempts).metrics;
    }
  }

  const record = {
    id: runId ? `gh-${runId}` : `local-${date}`,
    ...sideInfo(ref, sha, metrics, server, repo),
    date,
    run_url: runId ? `${server}/${repo}/actions/runs/${runId}` : "",
    env: {
      nvcpus: cpus().length,
      toolchain: env.TOOLCHAIN || "clang",
      build: env.BUILD || "release",
      iters: opts.iters,
      paired: ab,
    },
  };

  if (ab) {
    record.base = sideInfo(opts.baseRef || "base", opts.baseSha, baseMetrics, server, repo);
  }

  writeFileSync(opts.out, JSON.stringify(record, null, 2) + "\n");
  const summary = Object.entries(metrics).map(([g, m]) => `${g}=${Object.keys(m).length}`).join(", ");
  console.log(`wrote ${opts.out} (${summary}${ab ? ", paired" : ""})`);
}

main();
