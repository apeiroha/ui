import { createMemo } from "solid-js";
import { For, Show } from "@solidjs/web";

const W = 320;
const H = 150;
const PAD = 10;

interface Pt {
  x: number;
  y: number;
}

/**
 * A dependency-free sparkline. Points with no value are left as gaps and
 * the y-range is fit to the data that is present.
 *
 * `band` draws the spread of each sample (the median up to the 95th
 * percentile) behind the line: a single median hides how much the
 * samples moved, and a wide band is the visual form of "this metric
 * is too noisy to read a 5% change off".
 *
 * `marker` draws a vertical line at one index - used for the point
 * where the benchmark harness changed and the series is no longer
 * continuous.
 */
export function LineChart(props: {
  values: (number | null)[];
  color: string;
  band?: { lo: (number | null)[]; hi: (number | null)[] };
  marker?: number;
}) {
  const all = createMemo(() => {
    const nums = props.values.filter((v): v is number => v !== null);
    if (props.band) {
      for (const v of props.band.lo) if (v !== null) nums.push(v);
      for (const v of props.band.hi) if (v !== null) nums.push(v);
    }
    return nums;
  });

  const bounds = createMemo(() => {
    const nums = all();
    if (nums.length === 0) return null;
    let min = Math.min(...nums);
    let max = Math.max(...nums);
    if (min === max) {
      min -= 1;
      max += 1;
    }
    return { min, max };
  });

  const px = (i: number) => {
    const n = props.values.length;
    return n <= 1 ? W / 2 : PAD + (i * (W - 2 * PAD)) / (n - 1);
  };
  const py = (v: number) => {
    const b = bounds();
    if (!b) return H / 2;
    return H - PAD - ((v - b.min) * (H - 2 * PAD)) / (b.max - b.min);
  };

  const segments = createMemo(() => {
    if (!bounds()) return [] as Pt[][];
    const segs: Pt[][] = [];
    let cur: Pt[] = [];
    props.values.forEach((v, i) => {
      if (v === null) {
        if (cur.length) segs.push(cur);
        cur = [];
        return;
      }
      cur.push({ x: px(i), y: py(v) });
    });
    if (cur.length) segs.push(cur);
    return segs;
  });

  const dots = createMemo(() => {
    if (!bounds()) return [] as Pt[];
    const out: Pt[] = [];
    props.values.forEach((v, i) => {
      if (v !== null) out.push({ x: px(i), y: py(v) });
    });
    return out;
  });

  // The band is drawn as one filled polygon per contiguous run.
  const bandPaths = createMemo(() => {
    const band = props.band;
    if (!band || !bounds()) return [] as string[];
    const out: string[] = [];
    let cur: Pt[] = [];
    let curLo: Pt[] = [];
    const flush = () => {
      if (cur.length > 1) {
        const top = cur.map((p) => `${p.x},${p.y}`).join(" ");
        const bottom = curLo
          .slice()
          .reverse()
          .map((p) => `${p.x},${p.y}`)
          .join(" ");
        out.push(`${top} ${bottom}`);
      }
      cur = [];
      curLo = [];
    };
    const n = Math.min(band.lo.length, band.hi.length);
    for (let i = 0; i < n; i++) {
      const lo = band.lo[i];
      const hi = band.hi[i];
      if (lo === null || hi === null) {
        flush();
        continue;
      }
      cur.push({ x: px(i), y: py(hi) });
      curLo.push({ x: px(i), y: py(lo) });
    }
    flush();
    return out;
  });

  return (
    <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" class="chart">
      <Show when={props.band}>
        <For each={bandPaths()}>
          {(d) => (
            <polygon points={d} fill={props.color} fill-opacity="0.14" stroke="none" />
          )}
        </For>
      </Show>
      <For each={segments()}>
        {(seg) => (
          <polyline
            fill="none"
            stroke={props.color}
            stroke-width={seg.length > 1 ? 2 : 0}
            stroke-linecap="round"
            vector-effect="non-scaling-stroke"
            points={seg.map((p) => `${p.x},${p.y}`).join(" ")}
          />
        )}
      </For>
      <For each={dots()}>
        {(p) => <circle cx={p.x} cy={p.y} r="1.8" fill={props.color} vector-effect="non-scaling-stroke" />}
      </For>
      <Show when={props.marker !== undefined && props.marker >= 0}>
        <line
          x1={px(props.marker!)}
          x2={px(props.marker!)}
          y1={PAD / 2}
          y2={H - PAD / 2}
          stroke="#8b949e"
          stroke-width="1"
          stroke-dasharray="3 3"
          vector-effect="non-scaling-stroke"
        />
      </Show>
    </svg>
  );
}
