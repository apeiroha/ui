#!/usr/bin/env bash
# Verify the compare page's logic and that the tracking page is untouched.
#
# check_compare.mjs asserts the selection/diff rules that bench/compare.html
# implements (latest-per-ref, time-aligned, metric union, n/a for absent
# metrics, ns/op direction). It has to keep those rules in sync with the
# page by hand, so this also asserts the page still contains each rule.
#
# The tracking page is deliberately left alone, so any drift in its
# checksum is a failure, not a refresh.
set -euo pipefail

cd "$(dirname "$0")/.."

echo "=== compare page logic ==="
node scripts/check_compare.mjs

echo
echo "=== page/publish wiring ==="
fails=0
check() {
  if eval "$2"; then
    echo "  PASS  $1"
  else
    echo "  FAIL  $1"
    fails=$((fails + 1))
  fi
}

check "compare page has both selection modes" \
  "grep -q 'data-mode=\"latest\"' bench/compare.html && grep -q 'data-mode=\"aligned\"' bench/compare.html"
check "compare page picks latest per ref" \
  "grep -q 'function pickLatest' bench/compare.html"
check "compare page picks time-aligned" \
  "grep -q 'function pickAligned' bench/compare.html"
check "compare page has a same-machine pair mode" \
  "grep -q 'data-mode=\"pair\"' bench/compare.html"
check "compare page discovers runs with an embedded base" \
  "grep -q 'r.base && r.base.metrics' bench/compare.html"
check "compare page renders absent metrics as n/a" \
  "grep -q \"'n/a'\" bench/compare.html"
check "compare page states that lower ns/op is better" \
  "grep -q '↓ better' bench/compare.html"
check "compare page reads the same dataset as the tracking page" \
  "grep -q \"fetch('data/bench.json'\" bench/compare.html"
check "compare page links back to the tracking page" \
  "grep -q 'href=\"index.html\"' bench/compare.html"
check "compare page loads no external chart library" \
  "! grep -q 'cdn.jsdelivr.net' bench/compare.html"
check "publish copies compare.html alongside index.html" \
  "grep -q 'compare.html' scripts/bench_publish.sh"
check "bench workflow can target a ref" \
  "grep -q 'inputs.ref' .github/workflows/bench.yml"
check "push and schedule runs still use the pushed ref" \
  "grep -q 'github.ref_name' .github/workflows/bench.yml"

# Bench trigger + same-machine A/B wiring. The workflow resolves a head
# and a baseline from the event; the collector builds and runs both,
# interleaved, on one runner. Without the pull_request trigger every
# run.ref would be "main" (push only fires on main) and the compare page
# could not compare anything.
check "bench runs on pull_request" \
  "grep -qE '^  pull_request:' .github/workflows/bench.yml"
check "concurrency group is per-ref, not global" \
  "grep -q 'group: bench-pages-.*github.event.pull_request.head.ref' .github/workflows/bench.yml"
check "fork PRs skip the gh-pages publish" \
  "grep -q 'head.repo.full_name == github.repository' .github/workflows/bench.yml"

# Event -> baseline mapping. PR compares against the merge base
# (base.sha), main push against the previous tip (event.before), and the
# head ref is taken from the event rather than GITHUB_REF_NAME, which is
# "<pr>/merge" for a PR and would file every PR under a throwaway name.
check "PR head ref comes from the event, not the merge ref" \
  "grep -qF -- 'PR_HEAD_REF: \${{ github.event.pull_request.head.ref }}' .github/workflows/bench.yml"
check "PR baseline is the merge base (base.sha)" \
  "grep -qF -- 'PR_BASE_SHA: \${{ github.event.pull_request.base.sha }}' .github/workflows/bench.yml"
check "main push baseline is event.before" \
  "grep -qF -- 'BEFORE: \${{ github.event.before }}' .github/workflows/bench.yml"
check "an all-zero baseline is dropped" \
  "grep -qE 'zero=0{40}' .github/workflows/bench.yml"
check "collector supports an interleaved baseline build" \
  "grep -q 'base-bin-dir' scripts/bench_collect.mjs"
check "collector interleaves head and base for a pair" \
  "grep -q 'collectPair' scripts/bench_collect.mjs"
check "workflow builds the baseline into base-src" \
  "grep -q 'base-src/build' .github/workflows/bench.yml"
check "workflow records the checked-out head commit" \
  "grep -q 'steps.head.outputs.sha' .github/workflows/bench.yml"
check "workflow labels the run with the head ref, not the merge ref" \
  "grep -qF -- '--ref \"\$HEAD_REF\"' .github/workflows/bench.yml"

# The tracking page must not change in this branch.
if git diff --quiet main -- bench/index.html; then
  echo "  PASS  bench/index.html unchanged (tracking page untouched)"
else
  echo "  FAIL  bench/index.html changed -- the tracking page must stay as-is"
  fails=$((fails + 1))
fi

echo
if [ "$fails" -eq 0 ]; then
  echo "=== wiring: all checks passed ==="
else
  echo "=== wiring: $fails check(s) failed ==="
  exit 1
fi