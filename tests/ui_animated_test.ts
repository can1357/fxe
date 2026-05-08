// FXE resolves these package names through the host loader rather than node_modules.
import { CommandBuffer } from 'fxe';
import {
  Animated,
  Component,
  Draw,
  Layer,
  render,
  tickFrame,
  useAnimatedValue,
  useEffect,
} from 'fxe-ui';

import { assert, assertEqual, run, test } from './ts_harness.ts';

function assertNear(actual: number, expected: number, epsilon = 0.000001): void {
  assert(
    Math.abs(actual - expected) <= epsilon,
    `expected ${actual} to be within ${epsilon} of ${expected}`,
  );
}

type NumberAnimatedValue = {
  getValue(): number;
};

test('Animated.timing advances a value with linear easing', () => {
  const value = new Animated.Value(0);
  let callbackResult: boolean | undefined;

  Animated.timing(value, { to: 100, duration: 500, easing: 'linear' }).start((result) => {
    callbackResult = result.finished;
  });

  tickFrame(250);
  assertNear(value.getValue(), 50);
  tickFrame(250);
  assertNear(value.getValue(), 100);
  assertEqual(callbackResult, true);
});

test('Animated.Value.interpolate derives numeric-unit and hex color values', () => {
  const value = new Animated.Value(0);
  const color = value.interpolate({ inputRange: [0, 100], outputRange: ['#000', '#fff'] });
  const opacity = value.interpolate({ inputRange: [0, 100], outputRange: [0, 1] });
  const width = value.interpolate({ inputRange: [0, 100], outputRange: ['10px', '20px'] });

  assertEqual(color.getValue(), '#000000');
  value.setValue(50);
  assertEqual(color.getValue(), '#808080');
  assertNear(opacity.getValue(), 0.5);
  assertEqual(width.getValue(), '15px');
  value.setValue(100);
  assertEqual(color.getValue(), '#ffffff');
});

test('Animated.timing stop cancels and reports finished false once', () => {
  const value = new Animated.Value(0);
  const results: boolean[] = [];
  const animation = Animated.timing(value, { to: 100, duration: 500, easing: 'linear' });

  animation.start((result) => results.push(result.finished));
  tickFrame(125);
  const stoppedAt = value.getValue();
  animation.stop();
  animation.stop();
  tickFrame(1000);

  assertNear(value.getValue(), stoppedAt);
  assertEqual(results.length, 1);
  assertEqual(results[0], false);
});

test('starting a new animation interrupts the previous animation from the current value', () => {
  const value = new Animated.Value(0);
  const results: boolean[] = [];

  Animated.timing(value, { to: 100, duration: 1000, easing: 'linear' }).start((result) => {
    results.push(result.finished);
  });
  tickFrame(250);
  assertNear(value.getValue(), 25);

  Animated.timing(value, { to: 0, duration: 250, easing: 'linear' }).start((result) => {
    results.push(result.finished);
  });
  assertEqual(results.length, 1);
  assertEqual(results[0], false);

  tickFrame(125);
  assertNear(value.getValue(), 12.5);
  tickFrame(125);
  assertNear(value.getValue(), 0);
  assertEqual(results.length, 2);
  assertEqual(results[1], true);
});

test('Animated.spring uses an underdamped RK4 spring that can overshoot', () => {
  const value = new Animated.Value(0);
  let max = 0;
  value.addListener((next) => {
    max = Math.max(max, next);
  });

  Animated.spring(value, {
    to: 1,
    stiffness: 180,
    damping: 4,
    mass: 1,
    restThreshold: 0.0001,
  }).start();

  for (let i = 0; i < 120; ++i) tickFrame(16);

  assert(max > 1.01, `expected spring to overshoot target, max=${max}`);
});

test('useAnimatedValue returns a stable value and stops active animation on unmount', () => {
  let first: NumberAnimatedValue | null = null;
  let second: NumberAnimatedValue | null = null;
  const results: boolean[] = [];

  const Probe = Component(() => {
    const value = useAnimatedValue(0);
    if (first === null) first = value;
    second = value;
    useEffect(() => {
      Animated.timing(value, { to: 1, duration: 100, easing: 'linear' }).start((result) => {
        results.push(result.finished);
      });
    }, [value]);
    return Draw(() => undefined);
  }, 'AnimatedProbe');

  const target = new CommandBuffer();
  render(Layer({ children: [Probe({})] }), target);
  render(Layer({ children: [Probe({})] }), target);
  assert(first !== null, 'expected useAnimatedValue to initialize');
  const stableValue = first as unknown as NumberAnimatedValue;
  assert(Object.is(stableValue, second), 'useAnimatedValue should be stable across renders');

  tickFrame(50);
  assert(stableValue.getValue() > 0, 'expected animation to advance before unmount');
  const beforeUnmount = stableValue.getValue();
  render(Layer({ children: [] }), target);
  tickFrame(1000);

  assertNear(stableValue.getValue(), beforeUnmount);
  assertEqual(results.length, 1);
  assertEqual(results[0], false);
});

await run();
