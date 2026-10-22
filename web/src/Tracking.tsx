import { createMemo, createSignal, onSettled } from "solid-js";
import { For, Show } from "@solidjs/web";
import { loadBench } from "./data";
import { fmtDateTime, fmtNum } from "./format";
import { LineChart } from "./LineChart";
import {
  bandFor,
  groupsOf,
  harnessChangeIndex,
  headline,
  latestCv,
  metricNames,
  noiseClass,
  seriesFor,
  unitOf,
} from "./metrics";
import type { BenchData, Run } from "./types";

const PALETTE = ["#58a6ff", "#f0883e", "#3fb950", "#db61a2", "#a5d6ff", "#d29922", "#8b949e"];

function MetricCard(props: { runs: Run[]; group: string; name: string; color: string }) {
  const values = createMemo(() => seriesFor(props.runs, props.group, props.name));
  const band = createMemo(() => bandFor(props.runs, props.group, props.name));
  const marker = createMemo(() => harnessChangeIndex(props.runs));
  const unit = createMemo(() => unitOf(props.runs[props.runs.length - 1], props.group, props.name));
  const cv = createMemo(() => latestCv(props.runs, props.group, props.name));
  const noise = createMemo(() => noiseClass(cv()));

  const last = createMemo(() => {
    for (let i = props.runs.length - 1; i >= 0; i--) {
      const v = headline(props.runs[i], props.group, props.name);
      if (typeof v === "number") return v;
    }
    return null;
  });
  const prev = createMemo(() => {
    const seen: number[] = [];
    for (let i = props.runs.length - 1; i >= 0; i--) {
      const v = headline(props.runs[i], props.group, props.name);
      if (typeof v === "number") seen.push(v);
      if (seen.length > 1) break;
    }
    return seen.length > 1 ? seen[1] : null;
  });
  const delta = createMemo(() => {
    const a = last();
    const b = prev();
    if (a === null || b === null || b === 0) return null;
    const pct = ((a - b) / b) * 100;
    return { pct, cls: Math.abs(pct) < 0.5 ? "flat" : pct < 0 ? "good" : "bad" };
  });

  return (
    <div class="card">
      <div class="card-head">
        <div class="card-title">{props.name}</div>
        <div class="card-value">
          <Show when={last() !== null} fallback={<span>—</span>}>
            <span class="num">{fmtNum(last() as number, 1)}</span>
            <span class="unit"> {unit()}</span>
            <Show when={delta()}>
              {(d) => (
                <span class={"delta " + d().cls}>
                  {d().pct > 0 ? "+" : ""}
                  {d().pct.toFixed(1)}%
                </span>
              )}
            </Show>
            <Show when={cv() !== undefined}>
              <span class={"noise " + noise()} title="coefficient of variation of the samples">
                {noise() === "stable" ? "stable" : noise() === "fair" ? "fair" : "noisy"}
                <Show when={cv() !== undefined}> {(cv() as number).toFixed(1)}%</Show>
              </span>
            </Show>
          </Show>
        </div>
      </div>
      <div class="chart-wrap">
        <LineChart values={values()} color={props.color} band={band()} marker={marker()} />
      </div>
    </div>
  );
}

export function Tracking() {
  const [data, setData] = createSignal<BenchData | null>(null);
  const [error, setError] = createSignal<string | null>(null);
  const [group, setGroup] = createSignal("");

  onSettled(() => {
    void (async () => {
      try {
        const d = await loadBench();
        const gs = groupsOf(d.runs || []);
        setData(d);
        if (gs.length) setGroup(gs[0]);
      } catch (e) {
        setError((e as Error).message);
      }
    })();
  });

  const runs = createMemo(() => data()?.runs ?? []);
  const groups = createMemo(() => groupsOf(runs()));
  const cards = createMemo(() =>
    metricNames(runs(), group()).map((name, i) => ({ name, color: PALETTE[i % PALETTE.length] })),
  );
  const last = createMemo(() => runs()[runs().length - 1]);
  const marker = createMemo(() => harnessChangeIndex(runs()));

  return (
    <>
      <header>
        <h1>
          <a href="https://github.com/apeiroha/ui">ui (うい)</a> — benchmark tracking
        </h1>
        <div class="meta">
          <Show when={last()} fallback={<span>no runs yet</span>}>
            {(r) => (
              <>
                {runs().length} run(s) · latest <a href={r().commit_url}>{r().short || r().commit.slice(0, 8)}</a> @{" "}
                {fmtDateTime(r().date)} · {r().env?.toolchain ?? "?"} / {r().env?.build ?? "?"} / {r().env?.nvcpus ?? "?"} vCPU /{" "}
                {r().env?.count ?? r().env?.iters ?? "?"} samples
                <Show when={r().env?.cpu_model}> · {r().env?.cpu_model}</Show>
                <Show when={r().run_url}>
                  {" · "}
                  <a href={r().run_url}>workflow run</a>
                </Show>
              </>
            )}
          </Show>
        </div>
        <Show when={marker() >= 0 && marker() < runs().length - 1}>
          <div class="meta">
            the dashed line marks where the harness changed: before it every metric was a single
            number, after it the headline is a median of many samples.
          </div>
        </Show>
      </header>
      <main>
        <Show
          when={!error()}
          fallback={<div id="status">No benchmark data published yet ({error()}). Run the Bench workflow.</div>}
        >
          <Show when={data()} fallback={<div id="status">loading…</div>}>
            <div class="tabs">
              <For each={groups()}>
                {(g) => (
                  <button class={"tab" + (g === group() ? " active" : "")} onClick={() => setGroup(g)}>
                    {g}
                  </button>
                )}
              </For>
            </div>
            <Show when={cards().length > 0} fallback={<div id="status">no metrics for this group</div>}>
              <div class="grid">
                <For each={cards()}>
                  {(c) => <MetricCard runs={runs()} group={group()} name={c.name} color={c.color} />}
                </For>
              </div>
            </Show>
          </Show>
        </Show>
      </main>
    </>
  );
}
