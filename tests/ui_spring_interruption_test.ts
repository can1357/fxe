// FXE resolves these package names through the host loader rather than node_modules.
import { Animated } from 'fxe-ui';
import { tickAnimatedFrames } from '../packages/fxe-ui/src/animated/timing.ts';

import { assert, run, test } from './ts_harness.ts';

function assertNear(actual: number, expected: number, epsilon: number, label: string): void {
  assert(
    Math.abs(actual - expected) <= epsilon,
    `${label}: expected ${actual} to be within ${epsilon} of ${expected}`,
  );
}

function assertWithinPercent(
  actual: number,
  expected: number,
  tolerance: number,
  label: string,
): void {
  const epsilon = Math.abs(expected) * tolerance;
  assertNear(actual, expected, epsilon, label);
}

function measureVelocityAfterTick(value: { getValue(): number }, dtMs: number): number {
  const before = value.getValue();
  tickAnimatedFrames(dtMs);
  return (value.getValue() - before) / (dtMs / 1000);
}

test('Animated.spring preserves in-flight velocity when re-targeted', () => {
  const value = new Animated.Value(0);
  Animated.spring(value, { to: 100, stiffness: 170, damping: 26 }).start();

  let preRetargetVelocity = 0;
  for (let i = 0; i < 2000; ++i) {
    const velocity = measureVelocityAfterTick(value, 1);
    const position = value.getValue();
    if (position >= 45 && position <= 55 && velocity > 50) {
      preRetargetVelocity = velocity;
      break;
    }
  }

  assert(preRetargetVelocity > 0, 'expected spring to be moving before re-target');

  Animated.spring(value, { to: -50, stiffness: 170, damping: 26 }).start();
  const postRetargetVelocity = measureVelocityAfterTick(value, 1);

  assertWithinPercent(
    postRetargetVelocity,
    preRetargetVelocity,
    0.15,
    're-targeted spring should inherit velocity',
  );
});

test('Animated.spring settles after rapid irregular re-targets', () => {
  const restThreshold = 0.001;
  const value = new Animated.Value(0);
  const targets = [100, -100, 50, -50, 25] as const;
  const intervalsMs = [11, 23, 7, 19, 13] as const;

  for (let i = 0; i < targets.length; ++i) {
    Animated.spring(value, {
      to: targets[i],
      stiffness: 170,
      damping: 26,
      restThreshold,
    }).start();
    tickAnimatedFrames(intervalsMs[i]);
  }

  for (
    let i = 0;
    i < 2000 && Math.abs(value.getValue() - targets[targets.length - 1]) > restThreshold;
    ++i
  ) {
    tickAnimatedFrames(1);
  }

  assertNear(
    value.getValue(),
    targets[targets.length - 1],
    restThreshold,
    'rapid re-target spring should converge to final target',
  );
});

test('Animated.spring explicit velocity overrides inherited velocity', () => {
  const value = new Animated.Value(0);
  Animated.spring(value, { to: 100, stiffness: 170, damping: 26 }).start();

  for (let i = 0; i < 20; ++i) tickAnimatedFrames(1);
  assert(value.getValue() > 0, 'expected original spring to be active before override test');

  Animated.spring(value, { to: 0, stiffness: 170, damping: 26, velocity: 999 }).start();
  const explicitVelocity = measureVelocityAfterTick(value, 1);

  assertWithinPercent(
    explicitVelocity,
    999,
    0.15,
    'explicit spring velocity should win over inherited velocity',
  );
});

await run();
