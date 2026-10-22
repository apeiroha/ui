// Shape of gh-pages data/bench.json, written by scripts/bench_collect.mjs
// and scripts/bench_merge.mjs.

export interface MetricGroup {
  [metric: string]: number;
}

export interface MetricMap {
  [group: string]: MetricGroup;
}

/**
 * The samples of one metric, and the statistics derived from them.
 *
 * A record without `stats` is from before the benchmark harness kept
 * its samples: it carries a single number per metric and nothing else.
 */
export interface MetricStat {
  unit?: string;
  /** Operations the sample covered. */
  n?: number;
  /** Every measured repetition. */
  samples?: number[];
  min?: number;
  p50?: number;
  p95?: number;
  max?: number;
  mean?: number;
  stdev?: number;
  /** Coefficient of variation: how much the samples spread (%). */
  cv_pct?: number;
  nsamples?: number;
  /** Extra numbers a benchmark reported next to its main metric. */
  extra?: Record<string, { value: number; unit?: string }>;
}

export interface StatsMap {
  [group: string]: { [metric: string]: MetricStat };
}

/** The A/B verdict for one metric, computed by the collector. */
export interface MetricVerdict {
  verdict: "improved" | "regressed" | "unchanged" | "noisy";
  significant: boolean;
  p: number;
  delta_pct: number;
  base_p50: number;
  head_p50: number;
  base_cv_pct: number;
  head_cv_pct: number;
  n: number;
}

export interface VerdictMap {
  [group: string]: { [metric: string]: MetricVerdict };
}

/** Syscall counts per metric: a zero-noise signal next to the timings. */
export interface SyscallEntry {
  total?: Record<string, number>;
  per_op?: Record<string, number>;
  ops?: number;
}

export interface SyscallMap {
  [group: string]: { [metric: string]: SyscallEntry };
}

/** One measured revision: the head of a run, or an embedded baseline. */
export interface Side {
  ref: string;
  commit: string;
  short: string;
  commit_url: string;
  metrics: MetricMap;
  stats?: StatsMap;
}

export interface RunEnv {
  nvcpus?: number;
  toolchain?: string;
  build?: string;
  iters?: number;
  /** Measured repetitions per benchmark, per process run. */
  count?: number;
  paired?: boolean;
  /** The benchmark harness version: "v2" keeps the samples. */
  harness?: string;
  cpu_model?: string;
  kernel?: string;
  cpus_allowed?: string;
  runner?: string;
}

export interface Run extends Side {
  id: string;
  date: string;
  run_url?: string;
  env?: RunEnv;
  /** Present when the run measured a baseline on the same machine. */
  base?: Side;
  /** Present when the run measured a baseline: the A/B verdict. */
  verdict?: VerdictMap;
  syscalls?: SyscallMap;
}

export interface BenchData {
  generated_at?: string;
  runs: Run[];
}
