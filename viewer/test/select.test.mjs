// The selection module (js/ss_select.js) held to the native engine. Each case
// of select-fixture.json -- a motion report, every frame's score, what the
// pick rule reads -- is replayed through the exported plan, clock and pick
// calls in the order both decoders make them, and must give
// select-fixture.expected.json, which src/app/tests/select_parity_test.cpp
// holds the native build to. SS_SELECT_EXPECTED=write rewrites that file from
// this build; CI's native run is what confirms it.
//
//   node --test viewer/test/select.test.mjs

import assert from 'node:assert/strict';
import { readFileSync, writeFileSync } from 'node:fs';
import { test } from 'node:test';

const here = (p) => new URL(p, import.meta.url);
const wasm = readFileSync(here('../js/ss_select.wasm'));
const { default: SSSelectModule } = await import(here('../js/ss_select.js').href);
// The default loader fetches the .wasm beside the .js, which node cannot do
// for a file: URL.
const M = await SSSelectModule({
  instantiateWasm(imports, done) {
    WebAssembly.instantiate(wasm, imports).then((r) => done(r.instance));
    return {};
  },
});

const fixture = JSON.parse(readFileSync(here('select-fixture.json'), 'utf8'));
const expectedUrl = here('select-fixture.expected.json');
const writing = process.env.SS_SELECT_EXPECTED === 'write';
const expected = writing ? null : JSON.parse(readFileSync(expectedUrl, 'utf8'));

const VIEW = { planar: 0, packed360: 1, fisheye: 2 };
const KEPT = 0;

// The heap views are replaced when memory grows, so each is read after the
// allocation that may have grown it.
function copyIn(heap, values) {
  const p = M._malloc(4 * Math.max(1, values.length));
  M[heap].set(values, p / 4);
  return p;
}
const i32s = (p, n) => Array.from(M.HEAP32.subarray(p / 4, p / 4 + n));
const f32s = (p, n) => Array.from(M.HEAPF32.subarray(p / 4, p / 4 + n));

// A motion report's plan, as extract_frames makes it.
function planOf(m) {
  const cost = copyIn('HEAPF32', m.steps.map((s) => s.cost));
  const steps = copyIn('HEAPF32', m.steps.flatMap((s) => [...s.turn, s.drift]));
  const ends = copyIn('HEAP32', m.steps.map((s) => s.end));
  const n = M._sss_plan(cost, steps, ends, m.steps.length, m.frames, m.span, m.skip,
                        m.window, m.max_frames, m.fps, VIEW[m.view], m.out_fov,
                        m.adaptive_range);
  for (const p of [cost, steps, ends]) M._free(p);
  return i32s(M._sss_plan_ends(), n);
}

// decode_lockstep_ffmpeg's loop over every frame of the clip, with the
// fixture's numbers where the decoder would measure pixels.
function replay(c) {
  const plan = c.motion ? planOf(c.motion) : [];
  const ends = copyIn('HEAP32', plan);
  const clock = M._sss_clock_new(c.keep, c.skip, ends, plan.length, c.span[0], c.span[1]);
  M._free(ends);
  const rule = M._sss_pick_new(c.pick.max_blur, c.pick.neighbour ? 1 : 0,
                               c.pick.similar_bits, c.pick.lens);
  const judge = M._sss_pick_on(rule) === 1;
  const chosen = [];
  const kept = [];
  let held = -1;
  const end = c.span[1] >= 0 ? Math.min(c.span[1], c.frames) : c.frames;
  for (let i = c.span[0]; i < end; i++) {
    if (c.max_frames > 0 && kept.length >= c.max_frames) break;
    if (!M._sss_clock_candidate(clock, i)) continue;
    if (M._sss_clock_offer(clock, c.keep !== 0 ? c.scores[i] : 0)) held = i;
    if (!M._sss_clock_step(clock, i)) continue;
    let verdict = KEPT;
    let blur = -1;
    if (judge) {
      const hex = c.hashes[held];
      const width = c.pick.max_blur > 0 ? c.widths[held] : -1;
      verdict = M._sss_pick_judge(rule, held, width, parseInt(hex.slice(8), 16),
                                  parseInt(hex.slice(0, 8), 16));
      blur = M.HEAPF32[(M._sss_pick_last(rule) + 8) / 4];
    }
    chosen.push([held, verdict, blur]);
    if (verdict === KEPT) kept.push(held);
  }
  M._sss_clock_free(clock);
  M._sss_pick_free(rule);
  return { name: c.name, plan, chosen, kept };
}

// Value noise on an 8 px lattice plus a fine grain, in integers only, so the
// native test draws the same bytes.
function mix(x, y, seed) {
  let h = (Math.imul(x, 0x9e3779b1) ^ Math.imul(y, 0x85ebca77) ^ seed) >>> 0;
  h ^= h >>> 16;
  h = Math.imul(h, 0x7feb352d);
  h ^= h >>> 15;
  h = Math.imul(h, 0x846ca68b);
  h ^= h >>> 16;
  return h >>> 0;
}
function texel(u, v, seed) {
  const cx = u >> 3, cy = v >> 3, fx = u & 7, fy = v & 7;
  const top = (mix(cx, cy, seed) & 255) * (8 - fx) + (mix(cx + 1, cy, seed) & 255) * fx;
  const bot = (mix(cx, cy + 1, seed) & 255) * (8 - fx) + (mix(cx + 1, cy + 1, seed) & 255) * fx;
  const v8 = ((top * (8 - fy) + bot * fy) >> 6) + (mix(u, v, seed ^ 0x5bd1e995) & 15) - 8;
  return Math.min(255, Math.max(0, v8));
}

// The motion pass over a texture moved by the fixture's offsets, from the Y
// plane a decoder hands over to the plan.
function track(t) {
  const [sw, sh] = t.source;
  const tracker = M._sss_tracker_new(t.tracks, sw, sh);
  const o = M._sss_tracker_options(tracker) / 4;
  const [gw, gh] = [M.HEAP32[o + 1], M.HEAP32[o + 2]];
  const y = M._malloc(sw * sh);
  const grey = M._malloc(gw * gh);
  t.offsets.forEach(([ox, oy], k) => {
    const plane = M.HEAPU8.subarray(y, y + sw * sh);
    for (let r = 0; r < sh; r++)
      for (let c = 0; c < sw; c++) plane[r * sw + c] = texel(c + ox + 1000, r + oy + 1000, t.seed);
    M._sss_luma_box(y, sw, sh, gw, gh, grey);
    M._sss_tracker_track(tracker, grey, k * t.stride);
  });
  M._free(y);
  M._free(grey);
  M._sss_tracker_finish(tracker);
  const n = M._sss_tracker_count(tracker);
  const flat = f32s(M._sss_tracker_steps(tracker), 7 * n);
  const out = {
    view: M.HEAP32[o],
    grey: [gw, gh],
    weak: M._sss_tracker_weak_steps(tracker),
    ends: i32s(M._sss_tracker_ends(tracker), n),
    costs: f32s(M._sss_tracker_costs(tracker), n),
    steps: Array.from({ length: n }, (_, k) => flat.slice(7 * k, 7 * k + 7)),
  };
  const keep = M._sss_window_keep(t.keep, t.skip);
  const frames = t.offsets.length * t.stride;
  const planned = M._sss_tracker_plan(tracker, frames, 0, -1, t.skip, keep, 0, t.fps, t.range);
  out.plan = i32s(M._sss_plan_ends(), planned);
  M._sss_tracker_free(tracker);
  return out;
}

const got = { cases: fixture.cases.map(replay), track: track(fixture.track) };

if (writing) {
  writeFileSync(expectedUrl, JSON.stringify(got, null, 1).replace(/\n\s+(?=[-\d\]])/g, ' ') + '\n');
}

for (const [k, c] of got.cases.entries()) {
  test(`replay ${c.name}`, () => {
    if (writing) return;
    assert.deepEqual(c, expected.cases[k]);
  });
}

test('the motion pass over a moving texture', () => {
  const t = got.track;
  assert.equal(t.view, VIEW.planar);
  assert.ok(t.costs.length === fixture.track.offsets.length - 1);
  assert.ok(t.costs.every(Number.isFinite), 'every cost is finite');
  assert.ok(t.ends.every((e, k) => k === 0 || e > t.ends[k - 1]), 'ends ascend');
  const offs = fixture.track.offsets;
  const still = [];
  const moving = [];
  for (let k = 1; k < offs.length; k++)
    (offs[k][0] === offs[k - 1][0] && offs[k][1] === offs[k - 1][1] ? still : moving).push(t.costs[k - 1]);
  const mean = (v) => v.reduce((a, b) => a + b, 0) / v.length;
  assert.ok(mean(still) < mean(moving), `still ${mean(still)} < moving ${mean(moving)}`);
  assert.ok(t.plan.length > 0 && t.plan.every((e, k) => k === 0 || e > t.plan[k - 1]));
  if (!writing) assert.deepEqual(t, expected.track);
});
