// Pure selection/diff math for the dashboard. No DOM, no Solid: this is
// the testable spec (see metrics.test.ts) for what the tracking and
// compare pages render.
//
// All values are ns/op, lower is better, so a negative delta is "good".

import type { MetricMap, Run, Side } from "./types";

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
  const refs: string[] = [];
  for (const r of runs) if (r && r.ref && !refs.includes(r.ref)) refs.push(r.ref);
  return refs.sort();
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
    for (const n of Object.keys((side && side.metrics && side.metrics[group]) || {})) {
      if (!names.includes(n)) names.push(n);
    }
  }
  return names;
}

/** One metric across runs; missing points stay null so charts keep gaps. */
export function seriesFor(runs: Run[], group: string, metric: string): (number | null)[] {
  return runs.map((r) => {
    const g = r.metrics?.[group];
    return g && typeof g[metric] === "number" ? g[metric] : null;
  });
}

/** Last and previous present values. */
export function lastTwo(values: (number | null)[]): [number | null, number | null] {
  const present: number[] = [];
  for (const v of values) if (v !== null) present.push(v);
  if (present.length === 0) return [null, null];
  return [present[present.length - 1], present.length > 1 ? present[present.length - 2] : null];
}
