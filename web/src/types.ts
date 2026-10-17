// Shape of gh-pages data/bench.json, written by scripts/bench_collect.mjs
// and scripts/bench_merge.mjs.

export interface MetricGroup {
  [metric: string]: number;
}

export interface MetricMap {
  [group: string]: MetricGroup;
}

/** One measured revision: the head of a run, or an embedded baseline. */
export interface Side {
  ref: string;
  commit: string;
  short: string;
  commit_url: string;
  metrics: MetricMap;
}

export interface RunEnv {
  nvcpus?: number;
  toolchain?: string;
  build?: string;
  iters?: number;
  paired?: boolean;
}

export interface Run extends Side {
  id: string;
  date: string;
  run_url?: string;
  env?: RunEnv;
  /** Present when the run measured a baseline on the same machine. */
  base?: Side;
}

export interface BenchData {
  generated_at?: string;
  runs: Run[];
}
