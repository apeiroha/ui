import { createMemo, createSignal, onSettled } from "solid-js";
import { For, Show } from "@solidjs/web";
import { loadBench } from "./data";
import { fmtNum, fmtStamp } from "./format";
import {
  diffRow,
  groupsOf,
  headline,
  machineOf,
  pairRuns,
  pickAligned,
  pickLatest,
  refsOf,
  runsOf,
  sameMachine,
  unionMetrics,
  unitOf,
  verdictLabel,
  verdictOf,
} from "./metrics";
import type { BenchData, Run, Side, VerdictMap } from "./types";

type Mode = "latest" | "aligned" | "pair";

const MODES: { id: Mode; label: string }[] = [
  { id: "latest", label: "latest per ref" },
  { id: "aligned", label: "time-aligned" },
  { id: "pair", label: "same-machine pair" },
];

function Commit(props: { short?: string; date?: string }) {
  return (
    <span class="commit">
      <Show when={props.short}>
        <code>{props.short}</code>{" "}
      </Show>
      <Show when={props.date}>{fmtStamp(props.date)}</Show>
    </span>
  );
}

function NumCell(props: { v?: number }) {
  return (
    <Show when={typeof props.v === "number"} fallback={<td class="num na">n/a</td>}>
      <td class="num">{fmtNum(props.v as number)}</td>
    </Show>
  );
}

function MetricTable(props: {
  group: string;
  a?: Side;
  b?: Side;
  nameA: string;
  shortA?: string;
  nameB: string;
  shortB?: string;
  /** The A/B verdict, when the run measured a baseline. */
  verdict?: VerdictMap;
}) {
  const names = createMemo(() => unionMetrics(props.a, props.b, props.group));
  const hasVerdict = createMemo(() => Boolean(props.verdict?.[props.group]));
  return (
    <Show when={names().length > 0}>
      <h2>{props.group}</h2>
      <table>
        <thead>
          <tr>
            <th>metric (↓ better)</th>
            <th>
              {props.nameA} <Commit short={props.shortA} />
            </th>
            <th>
              {props.nameB} <Commit short={props.shortB} />
            </th>
            <th>Δ</th>
            <th>Δ%</th>
            <Show when={hasVerdict()}>
              <th>verdict</th>
            </Show>
          </tr>
        </thead>
        <tbody>
          <For each={names()}>
            {(name) => {
              const a = () => headline(props.a, props.group, name);
              const b = () => headline(props.b, props.group, name);
              const d = () => diffRow(a(), b());
              const unit = () => unitOf(props.b ?? props.a, props.group, name);
              const v = () => verdictOf({ verdict: props.verdict } as Run, props.group, name);
              const label = () => verdictLabel(v());
              return (
                <tr>
                  <td class="metric">
                    {name}
                    <span class="unit"> {unit()}</span>
                  </td>
                  <NumCell v={a()} />
                  <NumCell v={b()} />
                  <td class={"num " + d().cls}>
                    {d().present ? (d().delta! > 0 ? "+" : "") + fmtNum(d().delta!) : "—"}
                  </td>
                  <td class={"num " + d().cls}>
                    {d().present ? (d().pct! > 0 ? "+" : "") + d().pct!.toFixed(1) + "%" : "—"}
                  </td>
                  <Show when={hasVerdict()}>
                    <td class={"verdict " + label().cls}>
                      {label().text}
                      <Show when={v()}>
                        {(ver) => (
                          <span class="p">
                            {" "}
                            p={ver().p < 0.0001 ? "<0.0001" : ver().p.toFixed(4)}, n={ver().n}
                          </span>
                        )}
                      </Show>
                    </td>
                  </Show>
                </tr>
              );
            }}
          </For>
        </tbody>
      </table>
      <Show when={hasVerdict()}>
        <p class="hint">
          the verdict comes from a non-parametric test on the samples (Mann-Whitney U, p &lt; 0.05):
          a difference the test cannot separate from the noise is reported as "not significant".
        </p>
      </Show>
    </Show>
  );
}

function sideOf(s: Side): Side {
  return { ref: s.ref, commit: s.commit, short: s.short, commit_url: s.commit_url, metrics: s.metrics };
}

export function Compare() {
  const [data, setData] = createSignal<BenchData | null>(null);
  const [error, setError] = createSignal<string | null>(null);
  const [mode, setMode] = createSignal<Mode>("latest");
  const [refA, setRefA] = createSignal("");
  const [refB, setRefB] = createSignal("");
  const [pairIdx, setPairIdx] = createSignal(0);

  onSettled(() => {
    void (async () => {
      try {
        const d = await loadBench();
        const rs = refsOf(d.runs || []);
        setData(d);
        if (rs.length) setRefA(rs[0]);
        if (rs.length > 1) setRefB(rs[1]);
      } catch (e) {
        setError((e as Error).message);
      }
    })();
  });

  const runs = createMemo(() => data()?.runs ?? []);
  const refs = createMemo(() => refsOf(runs()));
  const pairs = createMemo(() => pairRuns(runs()));
  const pair = createMemo(() => pairs()[pairIdx()]);

  // Two-ref view (latest / aligned).
  const refView = createMemo(() => {
    const a = runsOf(runs(), refA());
    const b = runsOf(runs(), refB());
    if (!refA() || !refB()) return { status: "no benchmark runs recorded yet — run the Bench workflow", rows: null };
    if (refA() === refB()) return { status: "pick two different refs to compare", rows: null };
    if (a.length === 0 || b.length === 0) {
      const empty = a.length === 0 ? refA() : refB();
      return { status: `ref "${empty}" has no recorded run — trigger Bench with ref=${empty}`, rows: null };
    }
    const [ra, rb] = mode() === "aligned" ? pickAligned(a, b) : pickLatest(a, b);
    return { status: "", rows: { a: ra, b: rb } };
  });

  const refGroups = createMemo(() => {
    const r = refView().rows;
    return r ? groupsOf([r.a, r.b].filter((x): x is Run => !!x)) : [];
  });

  // Same-machine pair view: an embedded base vs its head.
  const pairView = createMemo(() => {
    const p = pair();
    if (!p || !p.base) return null;
    return { base: sideOf(p.base), head: sideOf(p), run: p };
  });
  const pairGroups = createMemo(() => {
    const v = pairView();
    return v ? groupsOf([v.base, v.head]) : [];
  });

  return (
    <>
      <header>
        <h1>
          <a href="https://github.com/apeiroha/ui">ui (うい)</a> — benchmark branch compare
        </h1>
        <div class="meta">
          <a href="index.html">← tracking</a>
          {" · "}
          <Show when={data()} fallback={<span>loading…</span>}>
            {runs().length} run(s), {refs().length} ref(s), {pairs().length} pair(s)
          </Show>
        </div>
      </header>
      <main>
        <Show
          when={!error()}
          fallback={<div id="status">No benchmark data published yet ({error()}). Run the Bench workflow.</div>}
        >
          <div class="controls">
            <Show when={mode() !== "pair"}>
              <div class="field">
                <label for="refA">Ref A (baseline)</label>
                <select id="refA" value={refA()} onChange={(e) => setRefA(e.currentTarget.value)}>
                  <For each={refs()}>{(r) => <option value={r}>{r}</option>}</For>
                </select>
              </div>
              <div class="field">
                <label for="refB">Ref B (compare)</label>
                <select id="refB" value={refB()} onChange={(e) => setRefB(e.currentTarget.value)}>
                  <For each={refs()}>{(r) => <option value={r}>{r}</option>}</For>
                </select>
              </div>
            </Show>
            <Show when={mode() === "pair"}>
              <div class="field">
                <label for="pair">Same-machine pair (base ← head)</label>
                <select id="pair" value={String(pairIdx())} onChange={(e) => setPairIdx(Number(e.currentTarget.value))}>
                  <For each={pairs()}>
                    {(p, i) => (
                      <option value={String(i())}>
                        {p.base?.ref ?? "base"} {p.base?.short ?? ""} → {p.ref} {p.short} @ {fmtStamp(p.date)}
                      </option>
                    )}
                  </For>
                </select>
              </div>
            </Show>
            <div class="field">
              <label>Selection</label>
              <div class="modes">
                <For each={MODES}>
                  {(m) => (
                    <button class={"mode" + (m.id === mode() ? " active" : "")} onClick={() => setMode(m.id)}>
                      {m.label}
                    </button>
                  )}
                </For>
              </div>
            </div>
          </div>

          <Show when={mode() === "pair"}>
            <Show
              when={pairView()}
              fallback={<div id="status">no same-machine A/B run recorded yet — open a PR or push to main</div>}
            >
              {(v) => (
                <>
                  <p class="hint">
                    same-machine pair: base and head were built from different commits and run interleaved on one runner, so the delta is
                    not machine drift.
                  </p>
                  <For each={pairGroups()}>
                    {(g) => (
                      <MetricTable
                        group={g}
                        a={v().base}
                        b={v().head}
                        verdict={v().run.verdict}
                        nameA={"base " + v().base.ref}
                        shortA={v().base.short}
                        nameB={"head " + v().head.ref}
                        shortB={v().head.short}
                      />
                    )}
                  </For>
                </>
              )}
            </Show>
          </Show>

          <Show when={mode() !== "pair"}>
            <p class="hint">
              {mode() === "aligned"
                ? "time-aligned: each side is the run nearest the shared midpoint of both refs’ history. Baselines may differ — compare the commits before reading the deltas."
                : "latest per ref: each side is that ref’s most recent run. Not necessarily measured at the same time."}
            </p>
            <Show when={refView().rows} fallback={<div id="status">{refView().status}</div>}>
              {(rows) => (
                <>
                  <Show when={!sameMachine(rows().a, rows().b)}>
                    <p class="warn">
                      the two sides were measured on different machines ({machineOf(rows().a)} vs{" "}
                      {machineOf(rows().b)}) - a shared CI runner is not the same machine twice, so
                      the deltas below are not a regression.
                    </p>
                  </Show>
                  <For each={refGroups()}>
                    {(g) => (
                      <MetricTable
                        group={g}
                        a={rows().a}
                        b={rows().b}
                        nameA={refA()}
                        shortA={rows().a?.short}
                        nameB={refB()}
                        shortB={rows().b?.short}
                      />
                    )}
                  </For>
                </>
              )}
            </Show>
          </Show>
        </Show>
      </main>
    </>
  );
}
