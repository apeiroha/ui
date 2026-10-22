// Statistics for the benchmark samples.
//
// The benchmarks keep every sample instead of collapsing to a single
// number, because a single ns/op on a shared runner is noise.  These
// helpers turn a sample into a summary (median, spread, stability) and
// turn two samples into a verdict (is the difference bigger than the
// noise?).
//
// The A/B test follows Go's benchstat: a non-parametric Mann-Whitney U
// test on the two samples, because benchmark timings are rarely
// normally distributed and a t-test assumes they are.

/** Sort ascending, without mutating the input. */
export function sorted(values) {
  return [...values].sort((a, b) => a - b);
}

/** Linear interpolation between order statistics (numpy's default). */
export function percentile(sortedValues, p) {
  const n = sortedValues.length;
  if (n === 0) return 0;
  if (n === 1) return sortedValues[0];
  const idx = p * (n - 1);
  const lo = Math.floor(idx);
  const hi = Math.ceil(idx);
  return sortedValues[lo] + (sortedValues[hi] - sortedValues[lo]) * (idx - lo);
}

/**
 * Summary of one sample.
 *
 * The headline is the median: the mean of a benchmark sample is pulled
 * around by the occasional scheduling hiccup, the median is not.
 * `cv_pct` is the coefficient of variation - the number that says
 * whether the metric is worth comparing at all (below ~5% it is stable,
 * above ~10% a difference needs a lot of evidence).
 */
export function summarize(values) {
  const n = values.length;
  if (n === 0) {
    return { n: 0, min: 0, p50: 0, p95: 0, max: 0, mean: 0, stdev: 0, cv_pct: 0 };
  }
  const s = sorted(values);
  const mean = s.reduce((a, b) => a + b, 0) / n;
  let acc = 0;
  for (const v of s) acc += (v - mean) * (v - mean);
  const stdev = n > 1 ? Math.sqrt(acc / (n - 1)) : 0;
  return {
    n,
    min: s[0],
    p50: percentile(s, 0.5),
    p95: percentile(s, 0.95),
    max: s[n - 1],
    mean,
    stdev,
    cv_pct: mean > 0 ? (stdev / mean) * 100 : 0,
  };
}

/* ── Binomial helpers (for the median's confidence interval) ── */

function binomPmf(n, k, p = 0.5) {
  let c = 1;
  for (let i = 0; i < k; i++) c = (c * (n - i)) / (i + 1);
  return c * Math.pow(p, k) * Math.pow(1 - p, n - k);
}

function binomCdf(n, k) {
  let sum = 0;
  for (let i = 0; i <= k; i++) sum += binomPmf(n, i);
  return sum;
}

/**
 * Distribution-free confidence interval for the median.
 *
 * The interval is [x(k), x(n+1-k)] with k chosen so that the coverage
 * of the interval is at least the requested confidence; the choice of k
 * comes from the binomial distribution of the rank of the median.  It
 * makes no normality assumption, which is what a benchmark sample
 * deserves.
 */
export function medianCI(values, confidence = 0.95) {
  const s = sorted(values);
  const n = s.length;
  if (n === 0) return [0, 0];
  if (n === 1) return [s[0], s[0]];
  const target = 1 - (1 - confidence) / 2;
  let k = 0;
  while (k < n - 1 && binomCdf(n, k) < target) k++;
  return [s[k], s[n - 1 - k]];
}

/* ── Mann-Whitney U ── */

function erf(x) {
  // Abramowitz & Stegun 7.1.26
  const sign = x < 0 ? -1 : 1;
  const ax = Math.abs(x);
  const t = 1 / (1 + 0.3275911 * ax);
  const y =
    1 -
    ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t +
      0.254829592) *
      t *
      Math.exp(-ax * ax);
  return sign * y;
}

function normalCdf(x) {
  return 0.5 * (1 + erf(x / Math.SQRT2));
}

/**
 * Two-sided Mann-Whitney U test.
 *
 * Returns the U statistic, the normal-approximation p-value and the
 * effect size (the probability that a random sample of `a` beats a
 * random sample of `b`, the "common language" effect size).  Ties are
 * corrected for; the sample sizes here (>= 10 per side) are large
 * enough for the approximation.
 */
export function mannWhitney(a, b) {
  const na = a.length;
  const nb = b.length;
  if (na === 0 || nb === 0) {
    return { u: 0, p: 1, effect: 0.5, n_a: na, n_b: nb };
  }
  const all = [...a.map((v) => ({ v, g: 0 })), ...b.map((v) => ({ v, g: 1 }))].sort(
    (x, y) => x.v - y.v,
  );
  // Average ranks over ties.
  const ranks = new Array(all.length);
  let tieSum = 0;
  let i = 0;
  while (i < all.length) {
    let j = i;
    while (j + 1 < all.length && all[j + 1].v === all[i].v) j++;
    const rank = (i + j) / 2 + 1;
    for (let k = i; k <= j; k++) ranks[k] = rank;
    const t = j - i + 1;
    tieSum += t * t * t - t;
    i = j + 1;
  }
  let r1 = 0;
  for (let k = 0; k < all.length; k++) if (all[k].g === 0) r1 += ranks[k];
  const u1 = r1 - (na * (na + 1)) / 2;
  const u = Math.min(u1, na * nb - u1);

  const mu = (na * nb) / 2;
  const n = na + nb;
  const tieTerm = tieSum > 0 ? tieSum / (12 * (n - 1)) : 0;
  const variance = (na * nb / 12) * (n + 1 - tieTerm);
  if (variance <= 0) {
    return { u, p: 1, effect: 0.5, n_a: na, n_b: nb };
  }
  // Continuity correction towards the mean, as benchstat applies it.
  // Without the "towards", two identical samples would come out
  // slightly *significant*, which is nonsense.
  const dev = Math.abs(u - mu);
  const z = (dev - 0.5) / Math.sqrt(variance);
  const p = Math.min(1, Math.max(0, 2 * (1 - normalCdf(Math.max(0, z)))));

  // Common-language effect size: P(a > b) + 0.5 P(a == b).
  let wins = 0;
  for (const x of a) {
    for (const y of b) {
      if (x > y) wins++;
      else if (x === y) wins += 0.5;
    }
  }
  const effect = wins / (na * nb);
  return { u, p, effect, n_a: na, n_b: nb };
}

/**
 * Compare two samples.
 *
 * `alpha` is the significance threshold (0.05, like benchstat's
 * default).  The verdict is one of:
 *   "improved"  - the second sample is significantly lower
 *   "regressed" - the second sample is significantly higher
 *   "unchanged" - no significant difference
 *   "noisy"     - either side is too noisy to draw a conclusion
 */
export function compare(a, b, { alpha = 0.05, noiseCvPct = 10 } = {}) {
  const sa = summarize(a);
  const sb = summarize(b);
  const mw = mannWhitney(a, b);
  const base = sb.p50 > 0 ? sa.p50 : 0;
  const deltaPct = base > 0 ? ((sb.p50 - sa.p50) / sa.p50) * 100 : 0;
  const tooNoisy = sa.cv_pct > noiseCvPct || sb.cv_pct > noiseCvPct;
  const significant = mw.p < alpha;

  let verdict;
  if (!significant) verdict = "unchanged";
  else if (sb.p50 < sa.p50) verdict = "improved";
  else verdict = "regressed";
  if (tooNoisy && verdict !== "unchanged") verdict = "noisy";

  return {
    verdict,
    significant,
    p: mw.p,
    effect: mw.effect,
    delta_pct: deltaPct,
    a: sa,
    b: sb,
    n_a: mw.n_a,
    n_b: mw.n_b,
  };
}
