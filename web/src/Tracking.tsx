import { createMemo, createSignal, onSettled } from "solid-js";
import { For, Show } from "@solidjs/web";
import { loadBench } from "./data";
import { fmtDateTime, fmtNum } from "./format";
import { LineChart } from "./LineChart";
import { groupsOf, lastTwo, metricNames, seriesFor } from "./metrics";
import type { BenchData, Run } from "./types";

const PALETTE = ["#58a6ff", "#f0883e", "#3fb950", "#db61a2", "#a5d6ff", "#d29922", "#8b949e"];

function MetricCard(props: { runs: Run[]; group: string; name: string; color: string }) {
  const values = createMemo(() => seriesFor(props.runs, props.group, props.name));
  const last2 = createMemo(() => lastTwo(values()));
  const delta = createMemo(() => {
    const [latest, prev] = last2();
    if (latest === null || prev === null || prev === 0) return null;
    const pct = ((latest - prev) / prev) * 100;
    return { pct, cls: Math.abs(pct) < 0.5 ? "flat" : pct < 0 ? "good" : "bad" };
  });

  return (
    <div class="card">
      <div class="card-head">
        <div class="card-title">{props.name}</div>
        <div class="card-value">
          <Show when={last2()[0] !== null} fallback={<span>—</span>}>
            <span class="num">{fmtNum(last2()[0] as number, 1)}</span>
            <span class="unit"> ns/op</span>
            <Show when={delta()}>
              {(d) => (
                <span class={"delta " + d().cls}>
                  {d().pct > 0 ? "+" : ""}
                  {d().pct.toFixed(1)}%
                </span>
              )}
            </Show>
          </Show>
        </div>
      </div>
      <div class="chart-wrap">
        <LineChart values={values()} color={props.color} />
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
                {fmtDateTime(r().date)} · {r().env?.toolchain ?? "?"} / {r().env?.build ?? "?"} / {r().env?.nvcpus ?? "?"} vCPU / min of{" "}
                {r().env?.iters ?? "?"}
                <Show when={r().run_url}>
                  {" · "}
                  <a href={r().run_url}>workflow run</a>
                </Show>
              </>
            )}
          </Show>
        </div>
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
