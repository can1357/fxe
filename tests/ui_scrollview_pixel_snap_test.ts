import { CommandBuffer } from 'fxe';
import {
  clearHitTargets,
  dispatchWheel,
  hitTargets,
  render,
  resetEventPipeline,
  ScrollView,
  Text,
} from 'fxe-ui';
import { __snapScrollOffsetForPaint } from '../packages/fxe-ui/src/components/ScrollView.ts';
import { assert, assertEqual, run, test } from './ts_harness.ts';

test('ScrollView paint offset snaps fractional wheel deltas to pixels', () => {
  assertEqual(__snapScrollOffsetForPaint({ x: 0.49, y: 12.5 }).x, 0);
  assertEqual(__snapScrollOffsetForPaint({ x: 0.49, y: 12.5 }).y, 13);
});

test('ScrollView keeps fractional logical offset but snaps painted child hit rects', () => {
  clearHitTargets();
  resetEventPipeline();

  let observedY = 0;
  const node = ScrollView({
    style: { width: 100, height: 40 },
    contentStyle: { width: 100, height: 200 },
    onScroll: (offset) => {
      observedY = offset.y;
    },
    children: Text({ style: { width: 80, height: 20 }, children: 'crisp' }),
  });

  const draw = () => {
    clearHitTargets();
    render(node, new CommandBuffer());
  };

  draw();
  dispatchWheel({
    type: 'wheel',
    x: 5,
    y: 5,
    dx: 0,
    dy: -0.01,
    modifiers: 0,
    phase: 'none',
    precision: false,
  });
  assert(
    Math.abs(observedY - 0.48) < 1e-9,
    `logical scroll offset ${observedY} should remain fractional`,
  );

  draw();
  const textTarget = hitTargets().find((target) => target.componentType === 'Text');
  assert(textTarget !== undefined, 'Text child hit target should be registered');
  assertEqual(textTarget.rect.y, 0, 'painted child rect should use snapped scroll projection');

  clearHitTargets();
  resetEventPipeline();
});

await run();
