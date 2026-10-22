#!/usr/bin/env node
// Run the ui benchmark binaries and emit a single JSON run record.
//
// Every metric keeps all of its samples, so the record carries a
// median, a spread and a stability figure instead of a single number.
// A single ns/op on a shared runner is noise; the samples are what
// makes a comparison between two builds meaningful (see
// scripts/benchstat.mjs, which turns them into a verdict).
//
// Same-machine A/B: pass --base-bin-dir to also measure a baseline
// build that was compiled from a different ref on this same runner. The
// two builds are then run *interleaved* (one round = head then base,
// order flipped every round) so machine drift over the job is shared by
// both sides instead of being attributed to whichever ran later. The
// baseline metrics are attached under `base` on the emitted record; the
// head stays at the top level so the tracking and per-ref compare pages
// keep working unchanged.

import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { cpus, tmpdir, release } from "node:os";
import { join, resolve } from "node:path";
import process from "node:process";
import { summarize } from "./bench_stats.mjs";

const GROUPS = [
  ["bench_ui", "bench_ui"],
  ["bench_ui_io", "bench_ui_io"],
];
// The pre-harness protocol: "  name   1234 ns/op  (N ops in Nus)".  Only
// used as a fallback for a baseline build that predates the JSON output.
const LINE = /^\s+(?<name>.*?\S)\s+(?<value>\d+(?:\.\d+)?)\s+ns\/op\b/;

function parseArgs(argv) {
  const opts = {
    binDir: "build",
    baseBinDir: "",
    iters: 2,
    count: 6,
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
    else if (argv[i] === "--count") opts.count = Number(argv[++i]);
    else if (argv[i] === "--attempts") opts.attempts = Number(argv[++i]);
    else if (argv[i] === "--out") opts.out = argv[++i];
    else if (argv[i] === "--ref") opts.ref = argv[++i];
    else if (argv[i] === "--sha") opts.sha = argv[++i];
    else if (argv[i] === "--base-ref") opts.baseRef = argv[++i];
    else if (argv[i] === "--base-sha") opts.baseSha = argv[++i];
  }
  return opts;
}

function round(v) {
  return Math.round(v * 1000) / 1000;
}

/* ── Machine fingerprint ──
 *
 * A shared runner is not the same machine twice: the CPU model, the
 * kernel and the CPU set all change between runs.  Recording them
 * makes it possible to refuse to compare numbers that were measured
 * on different hardware.
 */
function machineInfo() {
  const info = {};
  try {
    const cpuinfo = readFileSync("/proc/cpuinfo", "utf8");
    const m = cpuinfo.match(/^model name\s*:\s*(.+)$/m);
    if (m) info.cpu_model = m[1].trim();
  } catch {
    /* not Linux, or /proc not mounted */
  }
  info.kernel = release();
  try {
    const status = readFileSync("/proc/self/status", "utf8");
    const m = status.match(/^Cpus_allowed_list:\s*(.+)$/m);
    if (m) info.cpus_allowed = m[1].trim();
  } catch {
    /* ignore */
  }
  if (process.env.RUNNER_ENVIRONMENT) info.runner = process.env.RUNNER_ENVIRONMENT;
  return info;
}

/* ── Running one binary ── */

// One invocation of one binary.  Returns the list of benchmarks it
// measured (each with its samples), or null on a non-usable run (crash
// or empty output) so the caller can retry.  `side` only labels the
// warning; it distinguishes head from base in A/B mode.
function attemptRun(binDir, binary, count, side, tries) {
  const path = resolve(binDir, binary);
  const dir = mkdtempSync(join(tmpdir(), "uibench-"));
  const jsonPath = join(dir, "out.json");
  let stdout;
  try {
    stdout = execFileSync(path, ["--count", String(count), "--json", jsonPath], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
      timeout: 600000,
    });
  } catch (err) {
    // The runtime has a known intermittent scheduler race; a crashed run
    // yields no usable record, so retry rather than fail the whole job.
    const where = String(err.stdout || "")
      .trim()
      .split("\n")
      .filter(Boolean)
      .pop() || "";
    const how = err.signal ? `signal ${err.signal}` : `exit ${err.status}`;
    console.warn(
      `[warn] ${side} attempt ${tries} failed (${how})${
        where ? " last: " + where.trim() : ""
      }; retrying`,
    );
    rmSync(dir, { recursive: true, force: true });
    return null;
  }

  let parsed = null;
  try {
    parsed = JSON.parse(readFileSync(jsonPath, "utf8"));
  } catch {
    /* a baseline build that predates the JSON output */
  }
  rmSync(dir, { recursive: true, force: true });

  if (parsed && Array.isArray(parsed.benchmarks) && parsed.benchmarks.length > 0) {
    return parsed.benchmarks;
  }

  // Fallback: the old text protocol.  It yields a single sample per
  // metric, which cannot support a significance test, but it still
  // gives a number.
  const metrics = {};
  for (const line of String(stdout).split("\n")) {
    const m = LINE.exec(line);
    if (m) metrics[m.groups.name] = Number(m.groups.value);
  }
  const names = Object.keys(metrics);
  if (names.length === 0) {
    console.warn(`[warn] ${side} attempt ${tries} produced no metrics; retrying`);
    return null;
  }
  console.warn(
    `[warn] ${side} produced no JSON (pre-harness build?); using the legacy ` +
      `text protocol, so its metrics have a single sample and cannot be ` +
      `compared for significance`,
  );
  return names.map((name) => ({
    group: "bench_ui",
    name,
    unit: "ns/op",
    n: 0,
    samples: [metrics[name]],
  }));
}

/* ── Accumulating samples ── */

function accumulate(acc, benchmarks) {
  for (const b of benchmarks) {
    const key = `${b.group} ${b.name}`;
    let slot = acc.get(key);
    if (!slot) {
      slot = { group: b.group, name: b.name, unit: b.unit || "ns/op", samples: [], n: b.n };
      acc.set(key, slot);
    }
    if (Array.isArray(b.samples)) slot.samples.push(...b.samples);
    slot.unit = b.unit || slot.unit;
    slot.n = b.n || slot.n;
    if (b.extra) slot.extra = b.extra;
  }
}

/* Merge the per-benchmark slots of two collections (one per group). */
function mergeSlots(dst, src) {
  for (const slot of src.values()) {
    const key = `${slot.group} ${slot.name}`;
    let target = dst.get(key);
    if (!target) {
      target = { group: slot.group, name: slot.name, unit: slot.unit, samples: [], n: slot.n };
      dst.set(key, target);
    }
    target.samples.push(...slot.samples);
    target.unit = slot.unit || target.unit;
    target.n = slot.n || target.n;
    if (slot.extra) target.extra = slot.extra;
  }
}

function sideRecord(acc) {
  const metrics = {};
  const stats = {};
  for (const slot of acc.values()) {
    const s = summarize(slot.samples);
    metrics[slot.group] = metrics[slot.group] || {};
    metrics[slot.group][slot.name] = round(s.p50);
    stats[slot.group] = stats[slot.group] || {};
    stats[slot.group][slot.name] = {
      unit: slot.unit,
      n: slot.n,
      samples: slot.samples.map(round),
      min: round(s.min),
      p50: round(s.p50),
      p95: round(s.p95),
      max: round(s.max),
      mean: round(s.mean),
      stdev: round(s.stdev),
      cv_pct: round(s.cv_pct),
      nsamples: s.n,
    };
    if (slot.extra) stats[slot.group][slot.name].extra = slot.extra;
  }
  return { metrics, stats };
}

// Single ref: `iters` runs of the binary, all samples pooled.
function collectSingle(binDir, binary, iters, count, attempts) {
  const acc = new Map();
  let ok = 0;
  for (let tries = 1; tries <= attempts && ok < iters; tries++) {
    const parsed = attemptRun(binDir, binary, count, binary, tries);
    if (!parsed) continue;
    ok++;
    accumulate(acc, parsed);
  }
  if (ok === 0) throw new Error(`${binary}: no successful run in ${attempts} attempt(s)`);
  if (ok < iters) console.warn(`[warn] ${binary}: only ${ok}/${iters} successful run(s)`);
  return acc;
}

// Two refs, same machine: `iters` paired rounds. Both sides of a round
// must succeed or the whole round is retried, so every kept sample has
// a same-moment counterpart on the other side. Order flips each round
// to cancel any first-vs-second systematic bias.
function collectPair(headDir, baseDir, binary, iters, count, attempts) {
  const head = new Map();
  const base = new Map();
  let ok = 0;
  for (let tries = 1; tries <= attempts && ok < iters; tries++) {
    const headFirst = ok % 2 === 0;
    const [p1, s1] = headFirst ? [headDir, "head"] : [baseDir, "base"];
    const [p2, s2] = headFirst ? [baseDir, "base"] : [headDir, "head"];
    const r1 = attemptRun(p1, binary, count, `${binary}[${s1}]`, tries);
    const r2 = attemptRun(p2, binary, count, `${binary}[${s2}]`, tries);
    if (!r1 || !r2) continue;
    ok++;
    accumulate(s1 === "head" ? head : base, r1);
    accumulate(s2 === "head" ? head : base, r2);
  }
  if (ok === 0) throw new Error(`${binary}: no successful paired run in ${attempts} attempt(s)`);
  if (ok < iters) console.warn(`[warn] ${binary}: only ${ok}/${iters} paired run(s)`);
  return { head, base };
}

function sideInfo(ref, sha, metrics, stats, server, repo) {
  return {
    ref: ref || "local",
    commit: sha || "",
    short: sha ? sha.slice(0, 8) : "",
    commit_url: sha ? `${server}/${repo}/commit/${sha}` : "",
    metrics,
    stats,
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

  const headAcc = new Map();
  const baseAcc = new Map();
  if (ab) {
    for (const [group, binary] of GROUPS) {
      const { head, base } = collectPair(
        opts.binDir,
        opts.baseBinDir,
        binary,
        opts.iters,
        opts.count,
        opts.attempts,
      );
      mergeSlots(headAcc, head);
      mergeSlots(baseAcc, base);
    }
  } else {
    for (const [group, binary] of GROUPS) {
      mergeSlots(headAcc, collectSingle(opts.binDir, binary, opts.iters, opts.count, opts.attempts));
    }
  }

  const head = sideRecord(headAcc);
  const record = {
    id: runId ? `gh-${runId}` : `local-${date}`,
    ...sideInfo(ref, sha, head.metrics, head.stats, server, repo),
    date,
    run_url: runId ? `${server}/${repo}/actions/runs/${runId}` : "",
    env: {
      nvcpus: cpus().length,
      toolchain: env.TOOLCHAIN || "clang",
      build: env.BUILD || "release",
      iters: opts.iters,
      count: opts.count,
      paired: ab,
      harness: "v2",
      ...machineInfo(),
    },
  };

  if (ab) {
    const base = sideRecord(baseAcc);
    record.base = sideInfo(opts.baseRef || "base", opts.baseSha, base.metrics, base.stats, server, repo);
  }

  writeFileSync(opts.out, JSON.stringify(record, null, 2) + "\n");
  const summary = Object.entries(head.metrics)
    .map(([g, m]) => `${g}=${Object.keys(m).length}`)
    .join(", ");
  const sampleCount = Object.values(head.stats).reduce(
    (acc, group) => acc + Object.keys(group).length,
    0,
  );
  console.log(
    `wrote ${opts.out} (${summary}, ${sampleCount} metrics${ab ? ", paired" : ""})`,
  );
}

main();
