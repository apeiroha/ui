#!/usr/bin/env bash
# Publish the benchmark dashboard and append the new run to gh-pages.
#
# Usage: GITHUB_TOKEN=... GITHUB_REPOSITORY=owner/repo \
#          scripts/bench_publish.sh RUN.json
set -euo pipefail

run_json="${1:?usage: bench_publish.sh RUN.json}"
source_dir="${SOURCE_DIR:-bench}"
pages_branch="${PAGES_BRANCH:-gh-pages}"

remote="${PAGES_REMOTE:-}"
if [[ -z "$remote" ]]; then
    if [[ -z "${GITHUB_REPOSITORY:-}" ]]; then
        echo "GITHUB_REPOSITORY is required" >&2
        exit 1
    fi
    if [[ -z "${GITHUB_TOKEN:-}" ]]; then
        echo "GITHUB_TOKEN is required" >&2
        exit 1
    fi
    remote="https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
fi
pages_dir="${PAGES_DIR:-$(mktemp -d)}"

git config --global user.name "github-actions[bot]"
git config --global user.email "41898282+github-actions[bot]@users.noreply.github.com"

if git ls-remote --exit-code --heads "$remote" "$pages_branch" >/dev/null 2>&1; then
    git clone --depth 1 --branch "$pages_branch" "$remote" "$pages_dir"
else
    mkdir -p "$pages_dir"
    git -C "$pages_dir" init -q -b "$pages_branch"
    git -C "$pages_dir" remote add origin "$remote"
fi

cp "$source_dir/index.html" "$pages_dir/index.html"
touch "$pages_dir/.nojekyll"
node scripts/bench_merge.mjs "$run_json" "$pages_dir/data/bench.json"

git -C "$pages_dir" add -A
if git -C "$pages_dir" diff --cached --quiet; then
    echo "benchmark data unchanged; nothing to publish"
    exit 0
fi
git -C "$pages_dir" commit -q -m "bench: ${GITHUB_SHA:-local}"
git -C "$pages_dir" push -q origin "$pages_branch"
echo "published benchmark dashboard to $pages_branch"
