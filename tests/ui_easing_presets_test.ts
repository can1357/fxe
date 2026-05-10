import { Easings, cubicBezier, springPreset } from 'fxe-ui';
import { assert, assertEqual, assertThrows, run, test } from './ts_harness.ts';

test('cubicBezier materialStandard preserves endpoints and stays monotonic', () => {
  const easing = cubicBezier(0.4, 0, 0.2, 1);
  assert(Math.abs(easing(0) - 0) <= 1e-3, 'cubicBezier(0) should stay near 0');
  assert(Math.abs(easing(1) - 1) <= 1e-3, 'cubicBezier(1) should stay near 1');

  let previous = easing(0);
  for (let i = 1; i <= 10; ++i) {
    const next = easing(i / 10);
    assert(next > previous, `cubicBezier should be strictly increasing at sample ${i}`);
    previous = next;
  }
});

test('cubicBezier rejects invalid x control points', () => {
  assertThrows(
    () => cubicBezier(Number.NaN, 0, 0.2, 1),
    /Animated\.cubicBezier x1 must be a finite number/,
  );
  assertThrows(() => cubicBezier(2, 0, 0.2, 1), /Animated\.cubicBezier x1 must be between 0 and 1/);
  assertThrows(
    () => cubicBezier(0.4, 0, -1, 1),
    /Animated\.cubicBezier x2 must be between 0 and 1/,
  );
});

test('Easings exposes callable preset functions', () => {
  for (const [name, easing] of Object.entries(Easings)) {
    assertEqual(typeof easing, 'function');
    assert(Number.isFinite(easing(0.5)), `${name} should return a finite sample`);
  }
  assertEqual(Easings.linear(0.3), 0.3);
});

test('springPreset returns fresh preset objects', () => {
  const gentle = springPreset('gentle');
  assertEqual(gentle.stiffness, 120);
  gentle.stiffness = 999;
  assertEqual(springPreset('gentle').stiffness, 120);
});

test('springPreset rejects unknown names', () => {
  assertThrows(
    () => springPreset('does-not-exist' as never),
    /unknown spring preset: does-not-exist/,
  );
});

await run();
