#!/usr/bin/env node
// Compare two benchmark records and say whether the difference between
// them is real.
//
// The output follows Go's benchstat: a median with a confidence
// interval per side, the relative change, and a p-value from a
// non-parametric Mann-Whitney U test.  A difference that the test
// cannot distinguish from noise is marked "~" instead of being
// reported as a change - that is the whole point.  On a shared runner
// a "5% faster" line is usually machine drift, and a tool that cannot
// say so is worse than no tool.
//
//   node scripts/benchstat.mjs base.json head.json
//   node scripts/benchstat.mjs head.json              # summary only
//   node scripts/benchstat.mjs base.json head.json --markdown
//
// The records are the ones scripts/bench_collect.mjs writes: every
// metric carries its samples under `stats`.

import { readFileSync } from "node:fs";
import process from "node:process";
import { compare, medianCI, summarize } from "./bench_stats.mjs";

function parseArgs(argv) {
  const opts = { files: [], markdown: false, alpha: 0.05, minSamples: 10, threshold: 5 };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--markdown") opts.markdown = true;
    else if (argv[i] === "--alpha") opts.alpha = Number(argv[++i]);
    else if (argv[i] === "--min-samples") opts.minSamples = Number(argv[++i]);
    else if (argv[i] === "--threshold") opts.threshold = Number(argv[++i]);
    else opts.files.push(argv[i]);
  }
  return opts;
}

function load(path) {
  return JSON.parse(readFileSync(path, "utf8"));
}

/* The samples of one metric, wherever they live in the record. */
function samplesOf(record, group, name) {
  const st = record.stats && record.stats[group] && record.stats[group][name];
  if (st && Array.isArray(st.samples) && st.samples.length > 0) return st.samples;
  // A record without stats (an old one) has a single value per metric.
  const m = record.metrics && record.metrics[group] && record.metrics[group][name];
  return typeof m === "number" ? [m] : null;
}

function unitOf(record, group, name) {
  const st = record.stats && record.stats[group] && record.stats[group][name];
  return (st && st.unit) || "ns/op";
}

function fmt(v) {
  if (v === null || v === undefined) return "-";
  if (Math.abs(v) >= 1000) return v.toFixed(0);
  if (Math.abs(v) >= 100) return v.toFixed(1);
  if (Math.abs(v) >= 10) return v.toFixed(2);
  return v.toFixed(3);
}

function fmtP(p) {
  if (p === null || p === undefined) return "-";
  if (p < 0.0001) return "<0.0001";
  return p.toFixed(4);
}

/* ── Text output (benchstat style) ── */

function textOutput(base, head, opts) {
  const lines = [];
  const rows = [];

  if (base) {
    const nameW = Math.max(
      12,
      ...Object.keys(base.metrics || {}).flatMap((g) =>
        Object.keys(base.metrics[g] || {}).map((n) => n.length),
      ),
    );
    lines.push(
      "             │" +
        " base".padStart(Math.max(10, nameW - 8)) +
        "│" +
        " head".padStart(Math.max(10, nameW - 8)) +
        "│",
    );
    lines.push(
      "             │" +
        " ns/op".padStart(Math.max(10, nameW - 8)) +
        "│" +
        " ns/op".padStart(Math.max(10, nameW - 8)) +
        "│",
    );
  }

  for (const group of Object.keys(head.metrics || {})) {
    const names = Object.keys(head.metrics[group] || {});
    if (!names.length) continue;
    if (base) lines.push(`── ${group} ${"─".repeat(Math.max(0, 40 - group.length))}`);
    for (const name of names) {
      const headSamples = samplesOf(head, group, name);
      if (!headSamples) continue;
      const hs = summarize(headSamples);
      const unit = unitOf(head, group, name);
      const row = { group, name, unit, head: hs, base: null, cmp: null };

      if (base) {
        const baseSamples = samplesOf(base, group, name);
        if (baseSamples) {
          const bs = summarize(baseSamples);
          row.base = bs;
          row.cmp = compare(baseSamples, headSamples, { alpha: opts.alpha });
        }
      }
      rows.push(row);

      if (!base) {
        lines.push(
          `  ${name.padEnd(44)}${fmt(hs.p50).padStart(10)} ${unit.padEnd(8)} ` +
            `p50 (min ${fmt(hs.min)}, max ${fmt(hs.max)}, cv ${hs.cv_pct.toFixed(1)}%, n=${hs.n})`,
        );
        continue;
      }
      if (!row.base) {
        lines.push(
          `  ${name.padEnd(44)}${"-".padStart(10)} ${unit.padEnd(8)} ` +
            `no baseline sample (metric is new)`,
        );
        continue;
      }
      const c = row.cmp;
      const mark = c.significant ? " " : "~";
      const delta = `${c.delta_pct >= 0 ? "+" : ""}${c.delta_pct.toFixed(1)}%`;
      lines.push(
        `  ${name.padEnd(44)}${fmt(row.base.p50).padStart(10)} ${unit.padEnd(8)} ` +
          `-> ${fmt(hs.p50).padStart(10)} ${unit.padEnd(8)} ` +
          `${delta.padStart(8)}${mark} (p=${fmtP(c.p)}, n=${c.n_a}+${c.n_b})`,
      );
    }
  }

  const changed = rows.filter((r) => r.cmp && r.cmp.significant);
  const regressed = changed.filter((r) => r.cmp.verdict === "regressed");
  const improved = changed.filter((r) => r.cmp.verdict === "improved");
  const negligible = changed.filter((r) => r.cmp.verdict === "negligible");
  const noisy = rows.filter((r) => r.cmp && r.cmp.verdict === "noisy");
  const fewSamples = rows.filter(
    (r) => r.cmp && (r.cmp.n_a < opts.minSamples || r.cmp.n_b < opts.minSamples),
  );

  lines.push("");
  lines.push(
    `${rows.length} metric(s) compared: ${improved.length} improved, ` +
      `${regressed.length} regressed, ${negligible.length} changed but below ` +
      `${opts.threshold}%, ${rows.length - changed.length} unchanged`,
  );
  if (negligible.length) {
    lines.push(
      `  (${negligible.length} difference(s) are statistically significant but ` +
        `smaller than ${opts.threshold}% - a metric whose samples are very tight ` +
        `reports a 1% change as significant, which is noise, not a finding)`,
    );
  }
  if (noisy.length) {
    lines.push(
      `  (${noisy.length} of the changes are in metrics whose samples vary by ` +
        `more than 10% - treat them as noise)`,
    );
  }
  if (fewSamples.length) {
    lines.push(
      `  (${fewSamples.length} metric(s) have fewer than ${opts.minSamples} samples ` +
        `per side; the p-value is not reliable there)`,
    );
  }
  return { text: lines.join("\n"), rows };
}

/* ── Markdown output (for the CI job summary) ── */

function markdownOutput(base, head, opts) {
  const rows = [];
  for (const group of Object.keys(head.metrics || {})) {
    for (const name of Object.keys(head.metrics[group] || {})) {
      const headSamples = samplesOf(head, group, name);
      if (!headSamples) continue;
      const hs = summarize(headSamples);
      const row = { group, name, unit: unitOf(head, group, name), head: hs, base: null, cmp: null };
      if (base) {
        const baseSamples = samplesOf(base, group, name);
        if (baseSamples) {
          row.base = summarize(baseSamples);
          row.cmp = compare(baseSamples, headSamples, { alpha: opts.alpha });
        }
      }
      rows.push(row);
    }
  }

  const out = [];
  out.push("| metric | base p50 | head p50 | change | p | verdict |");
  out.push("|---|---:|---:|---:|---:|---|");
  for (const r of rows) {
    if (!r.base) {
      out.push(
        `| ${r.name} | - | ${fmt(r.head.p50)} ${r.unit} | - | - | new metric |`,
      );
      continue;
    }
    const c = r.cmp;
    const delta = `${c.delta_pct >= 0 ? "+" : ""}${c.delta_pct.toFixed(1)}%`;
    let verdict;
    if (!c.significant) verdict = "no significant change";
    else if (c.verdict === "regressed") verdict = "**regressed**";
    else if (c.verdict === "improved") verdict = "**improved**";
    else if (c.verdict === "negligible") verdict = `changed, but below ${opts.threshold}%`;
    else verdict = "changed, but noisy";
    out.push(
      `| ${r.name} | ${fmt(r.base.p50)} ${r.unit} | ${fmt(r.head.p50)} ${r.unit} | ` +
        `${delta} | ${fmtP(c.p)} | ${verdict} |`,
    );
  }

  const changed = rows.filter((r) => r.cmp && r.cmp.significant);
  const regressed = changed.filter((r) => r.cmp.verdict === "regressed");
  const improved = changed.filter((r) => r.cmp.verdict === "improved");
  const negligible = changed.filter((r) => r.cmp.verdict === "negligible");
  out.push("");
  out.push(
    `${rows.length} metric(s) compared · ${improved.length} improved · ` +
      `${regressed.length} regressed · ${negligible.length} changed but below ` +
      `${opts.threshold}% · ${rows.length - changed.length} unchanged`,
  );
  return out.join("\n");
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.files.length === 0) {
    console.error("usage: benchstat.mjs [base.json] head.json [--markdown]");
    process.exit(2);
  }
  const head = load(opts.files[opts.files.length - 1]);
  // A record produced with --base-bin-dir embeds its own baseline.
  let base = null;
  if (opts.files.length > 1) base = load(opts.files[0]);
  else if (head.base) base = head.base;

  if (opts.markdown) {
    console.log(markdownOutput(base, head, opts));
    return;
  }
  const { text, rows } = textOutput(base, head, opts);
  console.log(text);
  // A machine-readable line for the workflow to grep.
  for (const r of rows) {
    if (!r.cmp) continue;
    console.log(
      `BENCHSTAT ${r.group}/${r.name} ${r.cmp.verdict} ` +
        `delta=${r.cmp.delta_pct.toFixed(2)}% p=${r.cmp.p.toFixed(6)} ` +
        `base=${fmt(r.base.p50)} head=${fmt(r.head.p50)} n=${r.cmp.n_a}+${r.cmp.n_b}`,
    );
  }
}

main();
