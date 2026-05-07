import { assert, assertEqual, test } from './ts_harness.ts';

function assertFiniteNonNegative(value: unknown, label: string): asserts value is number {
  assert(typeof value === 'number', `${label} must be a number`);
  assert(Number.isFinite(value), `${label} must be finite`);
  assert(value >= 0, `${label} must be non-negative`);
}

function assertNear(actual: number, expected: number, tolerance: number, label: string): void {
  assert(
    Math.abs(actual - expected) <= tolerance,
    `${label} expected ${actual} to be within ${tolerance}ms of ${expected}`,
  );
}

test('performance.now returns a monotonic number', () => {
  const first: number = performance.now();
  const second: number = performance.now();
  const third: number = performance.now();

  assertFiniteNonNegative(first, 'first performance.now()');
  assertFiniteNonNegative(second, 'second performance.now()');
  assertFiniteNonNegative(third, 'third performance.now()');
  assert(second >= first, 'performance.now() must not move backward between immediate reads');
  assert(third >= second, 'performance.now() must not move backward across repeated reads');
});

test('performance.timeline records repeated marks with aggregate stats', () => {
  const name = 'bind-performance-test-repeated-marks';

  performance.timeline.beginMark(name);
  const firstDuration = performance.timeline.endMark(name);

  performance.timeline.beginMark(name);
  const secondDuration = performance.timeline.endMark(name);

  assertFiniteNonNegative(firstDuration, 'first endMark duration');
  assertFiniteNonNegative(secondDuration, 'second endMark duration');

  const snapshot = performance.timeline.snapshot();
  const mark = snapshot.marks[name];

  assert(mark !== undefined, 'snapshot must include completed mark');
  assertEqual(mark.count, 2, 'completed mark count');
  assertFiniteNonNegative(mark.totalMs, 'totalMs');
  assertFiniteNonNegative(mark.lastMs, 'lastMs');
  assertFiniteNonNegative(mark.minMs, 'minMs');
  assertFiniteNonNegative(mark.maxMs, 'maxMs');

  assertNear(mark.lastMs, secondDuration, 1, 'lastMs');
  assert(mark.totalMs >= mark.lastMs, 'totalMs must include lastMs');
  assert(mark.totalMs + 1 >= firstDuration + secondDuration, 'totalMs must include both durations');
  assert(mark.minMs <= mark.maxMs, 'minMs must not exceed maxMs');
  assert(mark.minMs <= firstDuration + 1, 'minMs must include first duration');
  assert(mark.minMs <= secondDuration + 1, 'minMs must include second duration');
  assert(mark.maxMs + 1 >= firstDuration, 'maxMs must include first duration');
  assert(mark.maxMs + 1 >= secondDuration, 'maxMs must include second duration');
});

test('performance.timeline silently skips unmatched endMark', () => {
  const name = 'bind-performance-test-unmatched-end';
  const endMark = performance.timeline.endMark as (markName: string) => unknown;

  const result = endMark(name);
  const snapshot = performance.timeline.snapshot();

  assertEqual(result, undefined, 'unmatched endMark result');
  assertEqual(
    snapshot.marks[name],
    undefined,
    'unmatched endMark must not create a completed mark',
  );
});
