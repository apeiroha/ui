// Pure selection/diff math for the dashboard. No DOM, no Solid: this is
// the testable spec (see metrics.test.ts) for what the tracking and
// compare pages render.
//
// All values are ns/op, lower is better, so a negative delta is "good".

import type { MetricMap, MetricStat, Run, Side, VerdictMap } from "./types";

/** Anything carrying a metric map: a run or an embedded baseline side. */
interface HasMetrics {
  metrics: MetricMap;
}

export type Verdict = "good" | "bad" | "flat";
export type DiffClass = Verdict | "na";

export interface Diff {
  present: boolean;
  cls: DiffClass;
  pct: number | null;
  delta: number | null;
}

/** Runs recorded for one ref, in history order. */
export function runsOf(runs: Run[], ref: string): Run[] {
  return runs.filter((r) => r && r.ref === ref);
}

/** Distinct refs, sorted. */
export function refsOf(runs: Run[]): string[] {
  const out: string[] = [];
  for (const r of runs) if (r && r.ref && !out.includes(r.ref)) out.push(r.ref);
  return out.sort();
}

/** Distinct metric group names, first-seen order. */
export function groupsOf(runs: readonly HasMetrics[]): string[] {
  const groups: string[] = [];
  for (const r of runs) {
    for (const g of Object.keys(r.metrics || {})) {
      if (!groups.includes(g)) groups.push(g);
    }
  }
  return groups;
}

/** Union of metric names within a group across runs, first-seen order. */
export function metricNames(runs: readonly HasMetrics[], group: string): string[] {
  const names: string[] = [];
  for (const r of runs) {
    const g = r.metrics?.[group] || {};
    for (const n of Object.keys(g)) if (!names.includes(n)) names.push(n);
  }
  return names;
}

/** Each side's most recent run. */
export function pickLatest(a: Run[], b: Run[]): [Run | undefined, Run | undefined] {
  return [a[a.length - 1], b[b.length - 1]];
}

/** Each side's run closest to the midpoint of both refs' combined history. */
export function pickAligned(a: Run[], b: Run[]): [Run | undefined, Run | undefined] {
  const all = a
    .concat(b)
    .map((r) => new Date(r.date).getTime())
    .filter((t) => !isNaN(t));
  if (all.length === 0) return pickLatest(a, b);
  const mid = (Math.min(...all) + Math.max(...all)) / 2;
  const nearest = (runs: Run[]): Run | undefined => {
    let best: Run | undefined;
    let bestD = Infinity;
    for (const r of runs) {
      const t = new Date(r.date).getTime();
      if (isNaN(t)) continue;
      const d = Math.abs(t - mid);
      if (d < bestD) {
        bestD = d;
        best = r;
      }
    }
    return best;
  };
  return [nearest(a), nearest(b)];
}

/** ns/op delta of b against a; absent sides render as n/a, never 0. */
export function diffRow(a: number | undefined, b: number | undefined): Diff {
  if (typeof a !== "number" || typeof b !== "number") {
    return { present: false, cls: "na", pct: null, delta: null };
  }
  const d = b - a;
  const pct = a === 0 ? 0 : (d / a) * 100;
  const cls: Verdict = Math.abs(pct) < 0.5 ? "flat" : pct < 0 ? "good" : "bad";
  return { present: true, cls, pct, delta: d };
}

/** Runs that embedded a same-machine baseline (bench_collect --base-bin-dir). */
export function pairRuns(runs: Run[]): Run[] {
  return runs.filter((r) => r && r.base && r.base.metrics);
}

/** Union of metric names within a group across two sides, first-seen. */
export function unionMetrics(a: Side | undefined, b: Side | undefined, group: string): string[] {
  const names: string[] = [];
  for (const side of [a, b]) {
    const g = (side && side.metrics && side.metrics[group]) || {};
    for (const n of Object.keys(g)) if (!names.includes(n)) names.push(n);
  }
  return names;
}

/* ── The samples behind a number ── */

/** The statistics of one metric in one run, if it has any. */
export function statOf(side: Side | undefined, group: string, name: string): MetricStat | undefined {
  return side?.stats?.[group]?.[name];
}

/**
 * The number to show for a metric.
 *
 * The median when the run has samples (a single average is pulled
 * around by the occasional scheduling hiccup), the recorded value
 * otherwise - records from before the harness kept its samples carry
 * only that.
 */
export function headline(side: Side | undefined, group: string, name: string): number | undefined {
  const st = statOf(side, group, name);
  if (st && typeof st.p50 === "number") return st.p50;
  const v = side?.metrics?.[group]?.[name];
  return typeof v === "number" ? v : undefined;
}

/** The unit of a metric ("ns/op", "MiB", "count", ...). */
export function unitOf(side: Side | undefined, group: string, name: string): string {
  return statOf(side, group, name)?.unit || "ns/op";
}

/** One metric across runs; missing points stay null so charts keep gaps. */
export function seriesFor(runs: Run[], group: string, metric: string): (number | null)[] {
  return runs.map((r) => {
    const v = headline(r, group, metric);
    return typeof v === "number" ? v : null;
  });
}

/**
 * The band between the median and the 95th percentile of each run.
 *
 * The median alone hides how spread the samples were; the band shows
 * it.  Runs without samples contribute nothing.
 */
export function bandFor(
  runs: Run[],
  group: string,
  metric: string,
): { lo: (number | null)[]; hi: (number | null)[] } {
  const lo: (number | null)[] = [];
  const hi: (number | null)[] = [];
  for (const r of runs) {
    const st = statOf(r, group, metric);
    if (st && typeof st.p50 === "number" && typeof st.p95 === "number") {
      lo.push(st.p50);
      hi.push(st.p95);
    } else {
      lo.push(null);
      hi.push(null);
    }
  }
  return { lo, hi };
}

/** Last and previous present values. */
export function lastTwo(values: (number | null)[]): [number | null, number | null] {
  const present: number[] = [];
  for (const v of values) if (v !== null) present.push(v);
  if (present.length === 0) return [null, null];
  return [present[present.length - 1], present.length > 1 ? present[present.length - 2] : null];
}

/* ── Stability ── */

export type NoiseClass = "stable" | "fair" | "noisy";

/**
 * How much a metric's samples spread, as a class.
 *
 * A metric whose samples vary by more than 10% cannot decide a 5%
 * difference, so the class is what tells a reader whether the delta
 * next to it means anything.
 */
export function noiseClass(cvPct: number | undefined): NoiseClass {
  if (cvPct === undefined || !isFinite(cvPct)) return "fair";
  if (cvPct < 5) return "stable";
  if (cvPct <= 10) return "fair";
  return "noisy";
}

/** The coefficient of variation of the latest run that has one. */
export function latestCv(runs: Run[], group: string, metric: string): number | undefined {
  for (let i = runs.length - 1; i >= 0; i--) {
    const st = statOf(runs[i], group, metric);
    if (st && typeof st.cv_pct === "number") return st.cv_pct;
  }
  return undefined;
}

/**
 * The index of the first run that used the sample-keeping harness.
 *
 * Before it every metric was a single number (the minimum of a few
 * runs); after it the headline is a median.  The two are close but not
 * the same, so the chart marks the change instead of pretending the
 * series is continuous.
 */
export function harnessChangeIndex(runs: Run[]): number {
  for (let i = 0; i < runs.length; i++) {
    if (runs[i]?.env?.harness === "v2") return i;
  }
  return -1;
}

/* ── The machine a run was measured on ── */

/** A fingerprint of the machine: the numbers a cross-run comparison needs. */
export function machineOf(run: Run | undefined): string {
  const env = run?.env;
  if (!env) return "";
  return [env.cpu_model ?? "", env.kernel ?? "", env.cpus_allowed ?? ""].join(" / ");
}

/**
 * Whether two runs were measured on the same machine.
 *
 * A shared CI runner is not the same machine twice: the CPU model, the
 * kernel and the CPU set all change between runs.  Numbers measured on
 * different machines must not be read as a regression.
 */
export function sameMachine(a: Run | undefined, b: Run | undefined): boolean {
  const ma = machineOf(a);
  const mb = machineOf(b);
  if (!ma || !mb) return true; // no fingerprint: do not claim a difference
  return ma === mb;
}

/* ── The A/B verdict ── */

/** The verdict for one metric of a paired run, if it has one. */
export function verdictOf(
  run: Run | undefined,
  group: string,
  name: string,
): VerdictMap[string][string] | undefined {
  return run?.verdict?.[group]?.[name];
}

/** A short label for a verdict, as shown next to the delta. */
export function verdictLabel(
  v: VerdictMap[string][string] | undefined,
): { text: string; cls: string } {
  if (!v) return { text: "", cls: "" };
  if (!v.significant) return { text: "not significant", cls: "flat" };
  if (v.verdict === "improved") return { text: "improved", cls: "good" };
  if (v.verdict === "regressed") return { text: "regressed", cls: "bad" };
  if (v.verdict === "negligible") return { text: "below the threshold", cls: "flat" };
  return { text: "noisy", cls: "flat" };
}
