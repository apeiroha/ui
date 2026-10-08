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

# Bench trigger wiring. Without a pull_request trigger every run.ref is
# "main" (bench_collect reads GITHUB_REF_NAME, and push only fires on
# main), so the compare page has a single selectable ref and cannot
# compare anything. GITHUB_REF_NAME is "<pr_number>/merge" for a PR, so
# the checkout must prefer github.head_ref to get a real branch name.
check "bench runs on pull_request" \
  "grep -qE '^  pull_request:' .github/workflows/bench.yml"
check "checkout prefers the PR head branch over the merge ref" \
  "grep -q 'inputs.ref || github.head_ref || github.ref_name' .github/workflows/bench.yml"
check "concurrency group is per-ref, not global" \
  "grep -q 'group: bench-pages-.*github.event.pull_request.head.ref' .github/workflows/bench.yml"
check "fork PRs skip the gh-pages publish" \
  "grep -q 'head.repo.full_name == github.repository' .github/workflows/bench.yml"

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