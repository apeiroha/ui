import { describe, expect, it } from "vitest";
import {
  bandFor,
  diffRow,
  groupsOf,
  harnessChangeIndex,
  headline,
  lastTwo,
  latestCv,
  machineOf,
  metricNames,
  noiseClass,
  pairRuns,
  pickAligned,
  pickLatest,
  refsOf,
  runsOf,
  sameMachine,
  seriesFor,
  unionMetrics,
  unitOf,
  verdictLabel,
  verdictOf,
} from "./metrics";
import type { MetricStat, Run, Side } from "./types";

const DROP = "rwlock write (4 goros, multi-vcpu)";

const BASE: Record<string, number> = {
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

function mk(id: string, ref: string, short: string, date: string, metrics: Record<string, number>, drop?: string): Run {
  const m: Record<string, number> = {};
  for (const [k, v] of Object.entries(metrics)) if (k !== drop) m[k] = v;
  return {
    id,
    ref,
    short,
    commit: short,
    commit_url: "",
    date,
    metrics: { bench_ui: m, bench_ui_io: { "ui_Read (always-available data)": 1468 } },
  };
}

function mkPair(
  id: string,
  ref: string,
  short: string,
  date: string,
  head: Record<string, number>,
  baseRef: string,
  baseShort: string,
  baseVals: Record<string, number>,
): Run {
  const base: Side = {
    ref: baseRef,
    commit: baseShort,
    short: baseShort,
    commit_url: "",
    metrics: { bench_ui: baseVals, bench_ui_io: { "ui_Read (always-available data)": 1500 } },
  };
  return {
    id,
    ref,
    short,
    commit: short,
    commit_url: "",
    date,
    metrics: { bench_ui: head, bench_ui_io: { "ui_Read (always-available data)": 1468 } },
    base,
  };
}

const featNew: Record<string, number> = {};
const featMid: Record<string, number> = {};
{
  const keys = Object.keys(BASE).filter((k) => k !== DROP);
  keys.forEach((k, i) => {
    featNew[k] = Math.round(BASE[k] * (i % 2 === 0 ? 0.85 : 1.3));
    featMid[k] = featNew[k];
  });
}

// same-machine pair: some metrics faster in head, the rest identical.
const pairHead: Record<string, number> = {};
const pairBase: Record<string, number> = {};
{
  const keys = Object.keys(BASE).filter((k) => k !== DROP);
  keys.forEach((k, i) => {
    pairBase[k] = BASE[k];
    pairHead[k] = i % 3 === 0 ? Math.round(BASE[k] * 0.8) : BASE[k];
  });
}

const runs: Run[] = [
  mk("gh-1", "main", "aaaaaaa1", "2026-10-01T03:30:00Z", BASE),
  mk("gh-2", "main", "bbbbbbb2", "2026-10-06T03:30:00Z", BASE),
  mk("gh-3", "main", "ccccccc3", "2026-10-08T03:30:00Z", BASE),
  mk("gh-4", "feat/x", "ddddd dd4", "2026-10-03T04:00:00Z", featMid, DROP),
  mk("gh-5", "feat/x", "fffffff6", "2026-10-05T04:00:00Z", featMid, DROP),
  mk("gh-6", "feat/x", "eeeeee e5", "2026-10-08T09:15:00Z", featNew, DROP),
  mkPair("gh-7", "feat/y", "1111111a", "2026-10-09T05:00:00Z", pairHead, "main", "2222222b", pairBase),
].sort((a, b) => a.date.localeCompare(b.date));

describe("ref selection", () => {
  it("discovers refs in sorted order", () => {
    expect(refsOf(runs)).toEqual(["feat/x", "feat/y", "main"]);
  });

  it("filters runs by ref", () => {
    expect(runsOf(runs, "nope")).toHaveLength(0);
    expect(runsOf(runs, "main").map((r) => r.id)).toEqual(["gh-1", "gh-2", "gh-3"]);
  });

  it("latest picks each side's newest run", () => {
    const [a, b] = pickLatest(runsOf(runs, "feat/x"), runsOf(runs, "main"));
    expect(a?.short).toBe("eeeeee e5");
    expect(b?.short).toBe("ccccccc3");
  });

  it("latest on an empty side is undefined", () => {
    expect(pickLatest([], [runs[0]])[0]).toBeUndefined();
  });

  it("aligned picks the mid-window run, not the newest", () => {
    const [a, b] = pickAligned(runsOf(runs, "feat/x"), runsOf(runs, "main"));
    expect(a?.short).toBe("fffffff6");
    expect(b?.short).toBe("bbbbbbb2");
  });

  it("aligned falls back to latest when no dates parse", () => {
    const bad = [{ ...runs[0], date: "not-a-date" }];
    expect(pickAligned(bad, []).length).toBe(2);
  });
});

describe("metric union", () => {
  it("keeps a metric only one side has", () => {
    const [a, b] = pickLatest(runsOf(runs, "feat/x"), runsOf(runs, "main"));
    const names = unionMetrics(a, b, "bench_ui");
    expect(names).toContain(DROP);
    expect(names).toHaveLength(Object.keys(BASE).length);
  });

  it("a metric missing on one side is absent, not 0", () => {
    const feat = runsOf(runs, "feat/x")[0];
    const main = runsOf(runs, "main")[0];
    expect(feat.metrics.bench_ui[DROP]).toBeUndefined();
    expect(typeof main.metrics.bench_ui[DROP]).toBe("number");
    const d = diffRow(feat.metrics.bench_ui[DROP], main.metrics.bench_ui[DROP]);
    expect(d.present).toBe(false);
    expect(d.cls).toBe("na");
  });

  it("metricNames unions across runs in a group", () => {
    expect(metricNames(runs, "bench_ui").length).toBe(Object.keys(BASE).length);
  });

  it("groupsOf lists groups in first-seen order", () => {
    expect(groupsOf(runs)).toEqual(["bench_ui", "bench_ui_io"]);
  });

  it("tolerates missing metrics", () => {
    expect(unionMetrics(undefined, undefined, "bench_ui")).toEqual([]);
  });
});

describe("diff direction (ns/op, lower is better)", () => {
  it("a faster b reads good, a slower b reads bad", () => {
    expect(diffRow(100, 85).cls).toBe("good");
    expect(diffRow(100, 130).cls).toBe("bad");
  });

  it("identical values are flat, not good/bad", () => {
    const d = diffRow(100, 100);
    expect(d.cls).toBe("flat");
    expect(d.pct).toBe(0);
  });

  it("a zero baseline does not produce Infinity/NaN", () => {
    expect(Number.isFinite(diffRow(0, 50).pct as number)).toBe(true);
  });

  it("reports delta and pct", () => {
    const d = diffRow(200, 150);
    expect(d.delta).toBe(-50);
    expect(d.pct).toBeCloseTo(-25);
  });
});

describe("same-machine pairs", () => {
  it("only runs carrying an embedded base qualify", () => {
    const pairs = pairRuns(runs);
    expect(pairs).toHaveLength(1);
    expect(pairs[0].id).toBe("gh-7");
    expect(pairRuns([runs[0]])).toHaveLength(0);
  });

  it("base vs head uses the same direction rule", () => {
    const p = pairRuns(runs)[0];
    const headKeys = Object.keys(p.metrics.bench_ui);
    const faster = headKeys.find((k) => p.metrics.bench_ui[k] < p.base!.metrics.bench_ui[k]);
    const same = headKeys.find((k) => p.metrics.bench_ui[k] === p.base!.metrics.bench_ui[k]);
    expect(faster).toBeDefined();
    expect(same).toBeDefined();
    expect(diffRow(p.base!.metrics.bench_ui[faster!], p.metrics.bench_ui[faster!]).cls).toBe("good");
    expect(diffRow(p.base!.metrics.bench_ui[same!], p.metrics.bench_ui[same!]).cls).toBe("flat");
  });
});

describe("series", () => {
  it("keeps gaps as null", () => {
    const s = seriesFor(runs, "bench_ui", DROP);
    expect(s.some((v) => v === null)).toBe(true);
    expect(s[0]).toBe(58);
  });

  it("lastTwo returns latest and previous present values", () => {
    expect(lastTwo([1, null, 3, 4])).toEqual([4, 3]);
    expect(lastTwo([null, null])).toEqual([null, null]);
    expect(lastTwo([7])).toEqual([7, null]);
  });
});

/* ── The sample-keeping harness ── */

function stat(p50: number, p95: number, cv: number): MetricStat {
  return { unit: "ns/op", n: 1000, samples: [p50, p95], min: p50, p50, p95, max: p95, cv_pct: cv, nsamples: 2 };
}

const v2Run: Run = {
  id: "gh-v2",
  ref: "main",
  short: "v2run00",
  commit: "v2run00",
  commit_url: "",
  date: "2026-10-20T03:30:00Z",
  metrics: { bench_ui: { "goroutine spawn+exit": 7000 } },
  stats: { bench_ui: { "goroutine spawn+exit": stat(7000, 7400, 2.5) } },
  env: { harness: "v2", cpu_model: "Intel Xeon", kernel: "6.8.0", cpus_allowed: "0-3" },
};

const v2Run2: Run = {
  ...v2Run,
  id: "gh-v2b",
  short: "v2run01",
  date: "2026-10-21T03:30:00Z",
  stats: { bench_ui: { "goroutine spawn+exit": stat(7100, 9000, 12) } },
};

describe("headline", () => {
  it("prefers the median when the run kept its samples", () => {
    expect(headline(v2Run, "bench_ui", "goroutine spawn+exit")).toBe(7000);
  });

  it("falls back to the recorded value for an older run", () => {
    expect(headline(runs[0], "bench_ui", "goroutine spawn+exit")).toBe(9046);
  });

  it("is undefined for a metric that is not there", () => {
    expect(headline(runs[0], "bench_ui", "nope")).toBeUndefined();
  });

  it("knows the unit", () => {
    expect(unitOf(v2Run, "bench_ui", "goroutine spawn+exit")).toBe("ns/op");
    expect(unitOf(runs[0], "bench_ui", "goroutine spawn+exit")).toBe("ns/op");
  });
});

describe("the band", () => {
  it("carries the median and the 95th percentile of each run", () => {
    const b = bandFor([v2Run, v2Run2], "bench_ui", "goroutine spawn+exit");
    expect(b.lo).toEqual([7000, 7100]);
    expect(b.hi).toEqual([7400, 9000]);
  });

  it("leaves a run without samples empty", () => {
    const b = bandFor([runs[0], v2Run], "bench_ui", "goroutine spawn+exit");
    expect(b.lo[0]).toBeNull();
    expect(b.lo[1]).toBe(7000);
  });
});

describe("stability", () => {
  it("classifies the coefficient of variation", () => {
    expect(noiseClass(1)).toBe("stable");
    expect(noiseClass(7)).toBe("fair");
    expect(noiseClass(25)).toBe("noisy");
    expect(noiseClass(undefined)).toBe("fair");
  });

  it("takes the cv of the newest run that has one", () => {
    expect(latestCv([v2Run, v2Run2], "bench_ui", "goroutine spawn+exit")).toBe(12);
    expect(latestCv([runs[0]], "bench_ui", "goroutine spawn+exit")).toBeUndefined();
  });
});

describe("the harness change", () => {
  it("finds the first run that kept its samples", () => {
    const list = [runs[0], v2Run, v2Run2];
    expect(harnessChangeIndex(list)).toBe(1);
  });

  it("is -1 when no run has it", () => {
    expect(harnessChangeIndex([runs[0]])).toBe(-1);
  });
});

describe("the machine", () => {
  it("fingerprints the cpu, the kernel and the cpu set", () => {
    expect(machineOf(v2Run)).toBe("Intel Xeon / 6.8.0 / 0-3");
    expect(machineOf(runs[0])).toBe("");
  });

  it("two runs from the same machine compare", () => {
    expect(sameMachine(v2Run, v2Run2)).toBe(true);
  });

  it("two runs from different machines do not", () => {
    const other = { ...v2Run2, env: { ...v2Run2.env, cpu_model: "AMD EPYC" } };
    expect(sameMachine(v2Run, other)).toBe(false);
  });

  it("a run without a fingerprint is not claimed to be a different machine", () => {
    expect(sameMachine(runs[0], v2Run)).toBe(true);
  });
});

describe("the A/B verdict", () => {
  const verdictRun: Run = {
    ...v2Run,
    verdict: {
      bench_ui: {
        "goroutine spawn+exit": {
          verdict: "improved",
          significant: true,
          material: true,
          p: 0.0001,
          delta_pct: -19.2,
          base_p50: 8670,
          head_p50: 7000,
          base_cv_pct: 2,
          head_cv_pct: 2.5,
          n: 24,
        },
      },
    },
  };

  it("is read from the run", () => {
    expect(verdictOf(verdictRun, "bench_ui", "goroutine spawn+exit")?.verdict).toBe("improved");
    expect(verdictOf(v2Run, "bench_ui", "goroutine spawn+exit")).toBeUndefined();
  });

  it("labels an improvement and a regression", () => {
    expect(verdictLabel(verdictOf(verdictRun, "bench_ui", "goroutine spawn+exit")).cls).toBe("good");
    const bad = { ...verdictRun, verdict: { bench_ui: { "goroutine spawn+exit": { ...verdictRun.verdict!.bench_ui["goroutine spawn+exit"], verdict: "regressed" as const } } } };
    expect(verdictLabel(verdictOf(bad, "bench_ui", "goroutine spawn+exit")).cls).toBe("bad");
  });

  it("a significant but tiny difference is below the practical threshold", () => {
    const small = {
      ...verdictRun,
      verdict: {
        bench_ui: {
          "goroutine spawn+exit": {
            ...verdictRun.verdict!.bench_ui["goroutine spawn+exit"],
            verdict: "negligible" as const,
            material: false,
            delta_pct: 1.5,
          },
        },
      },
    };
    const l = verdictLabel(verdictOf(small, "bench_ui", "goroutine spawn+exit"));
    expect(l.text).toBe("below the threshold");
    expect(l.cls).toBe("flat");
  });

  it("a difference the test cannot separate from noise is not significant", () => {
    const ns = { ...verdictRun, verdict: { bench_ui: { "goroutine spawn+exit": { ...verdictRun.verdict!.bench_ui["goroutine spawn+exit"], significant: false, verdict: "unchanged" as const } } } };
    const l = verdictLabel(verdictOf(ns, "bench_ui", "goroutine spawn+exit"));
    expect(l.text).toBe("not significant");
    expect(l.cls).toBe("flat");
  });
});
