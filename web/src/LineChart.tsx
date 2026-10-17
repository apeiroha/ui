import { createMemo } from "solid-js";
import { For } from "@solidjs/web";

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
 */
export function LineChart(props: { values: (number | null)[]; color: string }) {
  const bounds = createMemo(() => {
    const nums = props.values.filter((v): v is number => v !== null);
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

  return (
    <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" class="chart">
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
    </svg>
  );
}
