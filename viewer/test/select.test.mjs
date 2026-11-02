// The selection module (js/ss_select.js) held to the native engine: every case
// of select-fixture.json replayed by select-replay.mjs must give
// select-fixture.expected.json, which src/app/tests/select_parity_test.cpp
// holds the native build to. SS_SELECT_EXPECTED=write rewrites that file from
// this build; CI's native run is what confirms it.
//
//   node --test viewer/test/select.test.mjs

import assert from 'node:assert/strict';
import { readFileSync, writeFileSync } from 'node:fs';
import { test } from 'node:test';
import { conformance, instantiate, VIEW } from './select-replay.mjs';

const here = (p) => new URL(p, import.meta.url);
const { default: SSSelectModule } = await import(here('../js/ss_select.js').href);
const M = await instantiate(SSSelectModule, readFileSync(here('../js/ss_select.wasm')));
const { replay, track } = conformance(M);

const fixture = JSON.parse(readFileSync(here('select-fixture.json'), 'utf8'));
const expectedUrl = here('select-fixture.expected.json');
const writing = process.env.SS_SELECT_EXPECTED === 'write';
const expected = writing ? null : JSON.parse(readFileSync(expectedUrl, 'utf8'));

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
