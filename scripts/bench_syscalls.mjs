#!/usr/bin/env node
// Count the syscalls a benchmark makes.
//
// A latency number on a shared runner is noisy; a syscall count is
// not.  If a change adds one madvise() or one futex() to the spawn
// path, that shows up here as an exact number while the timing would
// be buried in the noise.
//
// The count is taken with strace -c (counting mode, not full tracing)
// around a single benchmark, so the numbers are per benchmark rather
// than an aggregate over the whole binary.
//
//   node scripts/bench_syscalls.mjs --bin-dir build --out syscalls.json
//
// The result lands in the record under "syscalls", one entry per
// metric: the raw counts and the same numbers divided by the
// benchmark's op count.

import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import process from "node:process";

const GROUPS = [
  ["bench_ui", "bench_ui"],
  ["bench_ui_io", "bench_ui_io"],
  ["bench_stack", "bench_stack"],
];

function parseArgs(argv) {
  const opts = {
    binDir: "build",
    out: "",
    probe: 2000,
    only: "",
    timeout: 600000,
    merge: "",
  };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--bin-dir") opts.binDir = argv[++i];
    else if (argv[i] === "--out") opts.out = argv[++i];
    else if (argv[i] === "--probe") opts.probe = Number(argv[++i]);
    else if (argv[i] === "--only") opts.only = argv[++i];
    else if (argv[i] === "--timeout") opts.timeout = Number(argv[++i]);
    else if (argv[i] === "--merge") opts.merge = argv[++i];
  }
  return opts;
}

// The syscall names worth keeping: the ones a change to the runtime
// would move.  Everything else (mmap, read, write, close, ...) is
// either constant or noise.
const INTERESTING = new Set([
  "futex",
  "madvise",
  "mprotect",
  "mmap",
  "munmap",
  "io_uring_enter",
  "io_uring_setup",
  "ppoll",
  "poll",
  "epoll_wait",
  "read",
  "write",
  "clone",
  "clone3",
  "sched_yield",
  "nanosleep",
  "clock_nanosleep",
  "getrusage",
  "membarrier",
]);

/* The strace -c summary: a header line, then rows of
 *
 *     %time  seconds  usecs/call  calls  errors  syscall
 *
 * where the errors column is blank when there were none, so the
 * syscall name is the last field and the call count the fourth. */
function parseStraceSummary(text) {
  const out = {};
  for (const line of text.split("\n")) {
    const f = line.trim().split(/\s+/);
    if (f.length < 5) continue;
    const calls = Number(f[3]);
    if (!Number.isFinite(calls)) continue;
    const name = f[f.length - 1];
    if (name === "total" || !INTERESTING.has(name)) continue;
    out[name] = calls;
  }
  return out;
}

// One benchmark, one strace run.  The binary is asked for a fixed
// number of operations (--probe) so that head and base are measured
// at the same size and the counts can be divided by it.
function probeBinary(binDir, binary, group, name, probe, timeout) {
  const path = resolve(binDir, binary);
  if (!existsSync(path)) return null;
  const dir = mkdtempSync(join(tmpdir(), "uisys-"));
  const trace = join(dir, "trace.txt");
  const json = join(dir, "out.json");
  try {
    execFileSync(
      "strace",
      [
        "-c",
        "-f",
        "-o", trace,
        path,
        "--only", `${group}/${name}`,
        "--probe", String(probe),
        "--json", json,
        "--count", "1",
      ],
      { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"], timeout },
    );
  } catch (err) {
    console.warn(`[warn] ${group}/${name}: strace run failed (${err.status ?? err.signal}); skipping`);
    rmSync(dir, { recursive: true, force: true });
    return null;
  }

  let counts = {};
  try {
    counts = parseStraceSummary(readFileSync(trace, "utf8"));
  } catch {
    /* no trace */
  }
  let ops = 0;
  try {
    const parsed = JSON.parse(readFileSync(json, "utf8"));
    const b = (parsed.benchmarks || []).find((x) => x.name === name);
    if (b) ops = b.n || 0;
  } catch {
    /* no json */
  }
  rmSync(dir, { recursive: true, force: true });

  const perOp = {};
  for (const [k, v] of Object.entries(counts)) {
    perOp[k] = ops > 0 ? Math.round((v / ops) * 1000) / 1000 : v;
  }
  return { counts, perOp, ops };
}

/**
 * Count the syscalls of every benchmark in the given build directory.
 * Returns { group: { name: { total, per_op, ops } } }.
 */
export function collectSyscalls(opts) {
  // The list of benchmarks comes from the binaries themselves, so this
  // stays in step with the driver without a hard-coded list.
  const result = {};
  for (const [group, binary] of GROUPS) {
    const path = resolve(opts.binDir, binary);
    if (!existsSync(path)) continue;
    let list = [];
    try {
      list = execFileSync(path, ["--list"], {
        encoding: "utf8",
        stdio: ["ignore", "pipe", "pipe"],
        timeout: 60000,
      })
        .split("\n")
        .map((l) => l.trim())
        .filter((l) => l.startsWith(group + "/"));
    } catch {
      continue;
    }
    for (const entry of list) {
      const name = entry.slice(group.length + 1);
      if (opts.only && name !== opts.only) continue;
      const r = probeBinary(opts.binDir, binary, group, name, opts.probe, opts.timeout);
      if (!r) continue;
      result[group] = result[group] || {};
      result[group][name] = { total: r.counts, per_op: r.perOp, ops: r.ops };
      const summary = Object.entries(r.perOp)
        .map(([k, v]) => `${k}=${v}`)
        .join(" ");
      console.log(`  ${group}/${name}: ${summary}`);
    }
  }

  return result;
}

function main() {
  const opts = parseArgs(process.argv.slice(2));

  // strace is what makes the counts possible; without it the step is
  // skipped rather than failed (a runner image without it should not
  // break the benchmark).
  try {
    execFileSync("strace", ["-V"], { stdio: "ignore" });
  } catch {
    console.warn("[warn] strace is not available; skipping the syscall counts");
    if (opts.merge) {
      const rec = JSON.parse(readFileSync(opts.merge, "utf8"));
      rec.syscalls = {};
      writeFileSync(opts.merge, JSON.stringify(rec, null, 2) + "\n");
    }
    return;
  }

  const result = collectSyscalls(opts);
  if (opts.out) {
    writeFileSync(opts.out, JSON.stringify(result, null, 2) + "\n");
    console.log(`wrote ${opts.out}`);
  }
  if (opts.merge) {
    const rec = JSON.parse(readFileSync(opts.merge, "utf8"));
    rec.syscalls = result;
    writeFileSync(opts.merge, JSON.stringify(rec, null, 2) + "\n");
    console.log(`merged the syscall counts into ${opts.merge}`);
  }
}

// Only run when invoked directly, not when imported by the collector.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main();
}
