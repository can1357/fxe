import { CommandBuffer } from 'fxe';
import {
  clearHitTargets,
  dispatchWheel,
  hitTest,
  Pressable,
  registerHitTarget,
  render,
  ScrollView,
  View,
} from 'fxe-ui';

import { assert, assertDeepEqual, run, test } from './ts_harness.ts';

function colorYRange(cb: CommandBuffer, color: number): { min: number; max: number } | null {
  const verts = cb.vertexBuffer();
  const words = new Uint32Array(verts.buffer, verts.byteOffset, verts.length);
  let min = Number.POSITIVE_INFINITY;
  let max = Number.NEGATIVE_INFINITY;
  let found = false;
  for (let i = 0; i < verts.length; i += 8) {
    if (words[i + 4] !== color) continue;
    const y = verts[i + 1];
    min = Math.min(min, y);
    max = Math.max(max, y);
    found = true;
  }
  return found ? { min, max } : null;
}

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
  dispatchWheel({
    type: 'wheel',
    dx: 4,
    dy: 10,
    modifiers: 0,
    phase: 'none',
    precision: false,
    x: 10,
    y: 10,
  });
  dispatchWheel({
    type: 'wheel',
    dx: -10,
    dy: -20,
    modifiers: 0,
    phase: 'none',
    precision: false,
    x: 10,
    y: 10,
  });
  assertDeepEqual(offsets, [
    { x: 4, y: 10 },
    { x: 0, y: 0 },
  ]);
});

test('ScrollView wheel uses native scroll direction and line-sized steps', () => {
  clearHitTargets();
  const offsets: Array<{ x: number; y: number }> = [];
  render(
    ScrollView({
      key: 'scroll-direction',
      style: { width: 100, height: 100 },
      contentStyle: { height: 300 },
      onScroll: (offset) => offsets.push(offset),
    }),
    new CommandBuffer(),
  );

  dispatchWheel({
    type: 'wheel',
    dx: 0,
    dy: -1,
    modifiers: 0,
    phase: 'none',
    precision: false,
    x: 10,
    y: 10,
  });
  dispatchWheel({
    type: 'wheel',
    dx: 0,
    dy: 1,
    modifiers: 0,
    phase: 'none',
    precision: false,
    x: 10,
    y: 10,
  });

  assertDeepEqual(offsets, [
    { x: 0, y: 48 },
    { x: 0, y: 0 },
  ]);
});

test('ScrollView paints scrollbar when content overflows', () => {
  clearHitTargets();
  const cb = new CommandBuffer();
  render(
    ScrollView({
      key: 'scrollbar-visible',
      style: { width: 100, height: 100 },
      contentStyle: { height: 300 },
    }),
    cb,
  );

  assert(cb.vertexCount() >= 8, 'overflowing ScrollView should paint track and thumb quads');
});

test('ScrollView background respects parent layout offset', () => {
  clearHitTargets();
  const cb = new CommandBuffer();
  render(
    View({
      key: 'scroll-position-root',
      style: { width: 100, height: 150 },
      children: [
        View({ key: 'toolbar', style: { width: 100, height: 50, backgroundColor: 0x0000ffff } }),
        ScrollView({
          key: 'offset-scroll',
          style: { width: 100, height: 100, backgroundColor: 0xff0000ff },
          contentStyle: { height: 200 },
        }),
      ],
    }),
    cb,
  );

  const toolbar = colorYRange(cb, 0xffff0000);
  const scrollBg = colorYRange(cb, 0xff0000ff);

  assert(toolbar !== null && toolbar.max <= 50, 'toolbar should remain above the scroll view');
  assert(scrollBg !== null && scrollBg.min >= 50, 'scroll background should start at its layout y');
});

test('ScrollView scrolls measured children without contentStyle', () => {
  clearHitTargets();
  const offsets: Array<{ x: number; y: number }> = [];
  render(
    ScrollView({
      key: 'scroll-measured-child',
      style: { width: 100, height: 100 },
      onScroll: (offset) => offsets.push(offset),
      children: View({
        key: 'tall-child',
        style: { width: 80, height: 300, backgroundColor: 0x224466ff },
      }),
    }),
    new CommandBuffer(),
  );

  dispatchWheel({
    type: 'wheel',
    dx: 0,
    dy: -1,
    modifiers: 0,
    phase: 'none',
    precision: false,
    x: 10,
    y: 10,
  });

  assertDeepEqual(offsets, [{ x: 0, y: 48 }]);
});

test('ScrollView moves painted content on rerender after wheel', () => {
  clearHitTargets();
  const root = () =>
    ScrollView({
      key: 'scroll-painted-content',
      style: { width: 100, height: 100, backgroundColor: 0x101010ff },
      children: [
        View({ key: 'red', style: { width: 80, height: 100, backgroundColor: 0xff0000ff } }),
        View({ key: 'green', style: { width: 80, height: 100, backgroundColor: 0x00ff00ff } }),
      ],
    });

  render(root(), new CommandBuffer());
  dispatchWheel({
    type: 'wheel',
    dx: 0,
    dy: -1,
    modifiers: 0,
    phase: 'none',
    precision: false,
    x: 10,
    y: 10,
  });

  clearHitTargets();
  const after = new CommandBuffer();
  render(root(), after);
  const red = colorYRange(after, 0xff0000ff);
  const green = colorYRange(after, 0xff00ff00);

  assert(red !== null && red.max <= 52, 'red band should shift upward after scroll');
  assert(green !== null && green.min >= 52, 'green band should enter the viewport after scroll');
});

test('ScrollView does not block child mouse targets', () => {
  clearHitTargets();
  const offsets: Array<{ x: number; y: number }> = [];
  render(
    ScrollView({
      key: 'scroll-child-targets',
      style: { width: 100, height: 100 },
      contentStyle: { height: 300 },
      onScroll: (offset) => offsets.push(offset),
      children: Pressable({
        key: 'child-pressable',
        style: { width: 80, height: 30 },
        onHoverIn: () => undefined,
      }),
    }),
    new CommandBuffer(),
  );

  assert(
    hitTest(10, 10)?.componentType === 'Pressable',
    'child Pressable should receive mouse hits',
  );
  dispatchWheel({
    type: 'wheel',
    dx: 0,
    dy: -1,
    modifiers: 0,
    phase: 'none',
    precision: false,
    x: 10,
    y: 10,
  });
  assertDeepEqual(offsets, [{ x: 0, y: 48 }]);
});

await run();
