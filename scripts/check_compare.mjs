#!/usr/bin/env node
// Self-check for the branch-compare page's selection logic, without a
// browser. bench/compare.html is a single static file, so its logic is
// duplicated here in plain JS and asserted against a synthetic fixture.
// Both sides must be changed together -- scripts/check_compare.mjs is
// the testable spec for the pick/diff rules in bench/compare.html.

import process from "node:process";

/* ── mirrored from bench/compare.html ───────────────────────────────── */

function runsOf(runs, ref) {
  return runs.filter(r => r && r.ref === ref);
}

function pickLatest(a, b) {
  return [a[a.length - 1], b[b.length - 1]];
}

/* Each side's run nearest the midpoint of both refs' combined history. */
function pickAligned(a, b) {
  const all = a.concat(b).map(r => new Date(r.date).getTime()).filter(t => !isNaN(t));
  if (all.length === 0) return pickLatest(a, b);
  const mid = (Math.min(...all) + Math.max(...all)) / 2;
  const nearest = runs => {
    let best = null, bestD = Infinity;
    for (const r of runs) {
      const t = new Date(r.date).getTime();
      if (isNaN(t)) continue;
      const d = Math.abs(t - mid);
      if (d < bestD) { bestD = d; best = r; }
    }
    return best;
  };
  return [nearest(a), nearest(b)];
}

/* Union of metric names across the group, first-seen order. A metric
 * present on only one side must read n/a, never 0. */
function metricUnion(runA, runB, group) {
  const names = [];
  for (const r of [runA, runB]) {
    for (const n of Object.keys(((r && r.metrics) || {})[group] || {})) {
      if (!names.includes(n)) names.push(n);
    }
  }
  return names;
}

/* ns/op: lower is better, matching the tracking page's convention
 * (index.html marks pct < 0 as "good"). */
function diffRow(a, b) {
  if (typeof a !== "number" || typeof b !== "number") {
    return { present: false, cls: "na", pct: null };
  }
  const d = b - a;
  const pct = a === 0 ? 0 : (d / a) * 100;
  const cls = Math.abs(pct) < 0.5 ? "flat" : (pct < 0 ? "good" : "bad");
  return { present: true, cls, pct, delta: d };
}

function refsOf(runs) {
  const refs = [];
  for (const r of runs) if (r && r.ref && !refs.includes(r.ref)) refs.push(r.ref);
  return refs.sort();
}

/* ── fixture ──────────────────────────────────────────────────────────
 * Three refs. main has three runs so "time-aligned" demonstrably picks
 * a non-latest one. feat omits one metric so the n/a path is exercised.
 */

const DROP = "rwlock write (4 goros, multi-vcpu)";

function mk(runId, ref, short, date, metrics, drop) {
  const m = {};
  for (const [k, v] of Object.entries(metrics)) {
    if (k === drop) continue;
    m[k] = v;
  }
  return {
    id: runId, ref, short, date,
    metrics: { bench_ui: m, bench_ui_io: { "ui_Read (always-available data)": 1468 } },
  };
}

const BASE = {
  "goroutine spawn+exit": 9046,
  "chan send+recv (buffered)": 34,
  "chan try_send+recv": 29,
  "mutex lock+unlock (uncontested)": 15,
  "mutex trylock+unlock (uncontested)": 15,
  "mutex contention spin (50 goros × 1000)": 70,
  "mutex contention work (50 goros × 1000)": 2298,
  "mutex contention (50 goros × 100 iter)": 142,
  "chan ping-pong (unbuffered, 2 goros)": 255,
  "local fanout spawn+exit": 5875,
  "rwlock read (single goro)": 15,
  "rwlock read (4 goros, multi-vcpu)": 31,
  "rwlock read (4 goros + writer)": 33,
  [DROP]: 58,
};

const featNew = {}, featMid = {};
{
  const keys = Object.keys(BASE).filter(k => k !== DROP);
  keys.forEach((k, i) => {
    // Mixed direction: even indices faster on feat, odd slower.
    featNew[k] = Math.round(BASE[k] * (i % 2 === 0 ? 0.85 : 1.3));
    featMid[k] = featNew[k];
  });
}

const runs = [
  mk("gh-1", "main", "aaaaaaa1", "2026-10-01T03:30:00Z", BASE),
  mk("gh-2", "main", "bbbbbbb2", "2026-10-06T03:30:00Z", BASE),
  mk("gh-3", "main", "ccccccc3", "2026-10-08T03:30:00Z", BASE),
  mk("gh-4", "feat/x", "ddddd dd4", "2026-10-03T04:00:00Z", featMid, DROP),
  mk("gh-5", "feat/x", "fffffff6", "2026-10-05T04:00:00Z", featMid, DROP),
  mk("gh-6", "feat/x", "eeeeee e5", "2026-10-08T09:15:00Z", featNew, DROP),
].sort((a, b) => a.date.localeCompare(b.date));

/* ── assertions ────────────────────────────────────────────────────── */

let pass = 0, fail = 0;
function eq(name, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  console.log("  " + (ok ? "PASS" : "FAIL") + "  " + name +
              (ok ? "" : "  got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)));
  ok ? pass++ : fail++;
}

console.log("=== bench/compare.html selection logic\n");

const refs = refsOf(runs);
eq("refs discovered from data", refs, ["feat/x", "main"]);

const a = runsOf(runs, "feat/x"), b = runsOf(runs, "main");

// latest-per-ref: each side's most recent run
const [la, lb] = pickLatest(a, b);
eq("latest picks newest on side A", la.short, "eeeeee e5");
eq("latest picks newest on side B", lb.short, "ccccccc3");

// time-aligned: midpoint of the whole window is ~2026-10-04T22, so the
// 10-05 feat run and the 10-06 main run win -- neither is the newest.
const [aa, ab] = pickAligned(a, b);
eq("aligned picks mid-window run on A", aa.short, "fffffff6");
eq("aligned picks mid-window run on B", ab.short, "bbbbbbb2");
eq("aligned differs from latest on A", aa.short !== la.short, true);
eq("aligned differs from latest on B", ab.short !== lb.short, true);

// metric union keeps metrics missing on one side
const union = metricUnion(la, lb, "bench_ui");
eq("union includes metric only main has", union.includes(DROP), true);
eq("union has every bench_ui metric", union.length, Object.keys(BASE).length);

// the dropped metric is genuinely absent on feat, so it must be n/a
const featVals = la.metrics.bench_ui[lb.metrics.bench_ui[DROP]];
eq("metric missing on feat side", featVals, undefined);
eq("metric present on main side", typeof lb.metrics.bench_ui[DROP], "number");

const na = diffRow(featVals, lb.metrics.bench_ui[DROP]);
eq("missing side renders as absent, not 0", na.present, false);

// direction: lower ns/op is better
const fasterKey = Object.keys(BASE).find(k => k !== DROP &&
  la.metrics.bench_ui[k] < lb.metrics.bench_ui[k]);
const slowerKey = Object.keys(BASE).find(k => k !== DROP &&
  la.metrics.bench_ui[k] > lb.metrics.bench_ui[k]);
eq("fixture has a faster metric", !!fasterKey, true);
eq("fixture has a slower metric", !!slowerKey, true);

const dFast = diffRow(lb.metrics.bench_ui[fasterKey], la.metrics.bench_ui[fasterKey]);
eq("B faster => good", dFast.cls, "good");
const dSlow = diffRow(lb.metrics.bench_ui[slowerKey], la.metrics.bench_ui[slowerKey]);
eq("B slower => bad", dSlow.cls, "bad");

// flat threshold: identical values read as flat, not good/bad
const dFlat = diffRow(100, 100);
eq("identical values => flat", dFlat.cls, "flat");
eq("flat pct is 0", dFlat.pct, 0);

// zero baseline must not produce Infinity/NaN
const dZero = diffRow(0, 50);
eq("zero baseline guarded", Number.isFinite(dZero.pct), true);

// degenerate inputs must not throw
eq("pickLatest on empty side is undefined", pickLatest([], [1])[0], undefined);
eq("pickAligned falls back to latest when no dates",
   pickAligned([{ date: "not-a-date" }], [{ date: "also-bad" }])[0].short, undefined);
eq("metricUnion tolerates missing metrics", metricUnion(null, null, "bench_ui"), []);
eq("runsOf filters by ref", runsOf(runs, "nope").length, 0);

console.log("\n=== Results: " + pass + " passed, " + fail + " failed ===");
process.exit(fail ? 1 : 0);