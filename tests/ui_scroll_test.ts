import { clearHitTargets, dispatchWheel, registerHitTarget } from 'fxe-ui';

import { assertDeepEqual, run, test } from './ts_harness.ts';

test('wheel dispatch forwards deltas to scroll target', () => {
  clearHitTargets();
  const offsets: Array<{ x: number; y: number }> = [];
  let state = { x: 0, y: 0 };
  registerHitTarget({
    id: 'scroll',
    rect: {
      x: 0,
      y: 0,
      width: 100,
      height: 100,
      paddingLeft: 0,
      paddingTop: 0,
      paddingRight: 0,
      paddingBottom: 0,
      children: [],
    },
    onWheel: (ev: { dx: number; dy: number }) => {
      state = { x: Math.max(0, state.x + ev.dx), y: Math.max(0, state.y + ev.dy) };
      offsets.push(state);
    },
  });
  dispatchWheel({ type: 'wheel', dx: 4, dy: 10, modifiers: 0, x: 10, y: 10 });
  dispatchWheel({ type: 'wheel', dx: -10, dy: -20, modifiers: 0, x: 10, y: 10 });
  assertDeepEqual(offsets, [
    { x: 4, y: 10 },
    { x: 0, y: 0 },
  ]);
});

await run();
