import { CommandBuffer } from 'fxe';
import {
  Minimap,
  clearHitTargets,
  dispatchMouseDown,
  dispatchMouseMove,
  hitTest,
  render,
  resetEventPipeline,
} from 'fxe-ui';

import {
  __buildMinimapBuckets,
  __computeViewportRect,
} from '../packages/fxe-ui/src/components/Minimap.ts';
import { assert, assertEqual, run, test } from './ts_harness.ts';

test('Minimap renders without throwing for an empty doc', () => {
  const cb = new CommandBuffer();
  render(
    Minimap({
      doc: new TextDocument(''),
      lineHeight: 18,
      viewportHeight: 120,
      scrollOffset: 0,
      style: { width: 80, height: 120 },
    }),
    cb,
  );
  assert(cb.vertexCount() >= 0);
});

test('Minimap rebuilds rects when the document revision advances', () => {
  const doc = new TextDocument('alpha beta\n  gamma\n');
  const before = bucketSignature(
    __buildMinimapBuckets({
      doc,
      scale: 0.5,
      lineHeight: 10,
      width: 80,
      height: 40,
      defaultColor: 0x778899ff,
    }),
  );
  doc.replace(0, 5, 'z');
  const after = bucketSignature(
    __buildMinimapBuckets({
      doc,
      scale: 0.5,
      lineHeight: 10,
      width: 80,
      height: 40,
      defaultColor: 0x778899ff,
    }),
  );
  assert(before !== after, 'bucket cache input should change after a document edit');
});

test('viewport indicator position is proportional to scrollOffset and content height', () => {
  const rect = __computeViewportRect({
    scrollOffset: 36,
    viewportHeight: 54,
    contentHeight: 180,
    width: 80,
    scale: 0.15,
  });
  assert(Math.abs(rect.y - 5.4) < 1e-9, 'viewport y should scale with scroll offset');
  assert(Math.abs(rect.height - 8.1) < 1e-9, 'viewport height should scale with viewport height');
  assertEqual(rect.width, 80);
});

test('onScrollRequest fires with the expected offset when a drag is simulated', () => {
  clearHitTargets();
  resetEventPipeline();
  const requested: number[] = [];
  const node = Minimap({
    doc: new TextDocument('aa\nbb\ncc\ndd'),
    lineHeight: 10,
    viewportHeight: 20,
    scrollOffset: 0,
    scale: 0.5,
    onScrollRequest: (offset) => {
      requested.push(offset);
    },
    style: { width: 80, height: 20 },
  });

  render(node, new CommandBuffer());
  render(node, new CommandBuffer());
  assert(hitTest(5, 10)?.componentType === 'Minimap', 'minimap drag target should be registered');

  dispatchMouseDown({ type: 'mousedown', x: 5, y: 10, button: 0, modifiers: 0 });
  dispatchMouseMove({ type: 'mousemove', x: 5, y: 15, dx: 0, dy: 5, modifiers: 0 });

  assertEqual(requested[0], 10);
  assertEqual(requested.at(-1), 20);

  clearHitTargets();
  resetEventPipeline();
});

await run();

function bucketSignature(buckets: readonly { color: number; rects: Float32Array }[]): string {
  return buckets.map((bucket) => `${bucket.color}:${Array.from(bucket.rects).join(',')}`).join('|');
}
