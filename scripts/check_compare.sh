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