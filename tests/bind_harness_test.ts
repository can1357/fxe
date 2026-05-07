import {
  assert,
  assertDeepEqual,
  assertEqual,
  assertRejects,
  assertThrows,
  test,
} from './ts_harness.ts';

test('harness assert helpers', () => {
  assert(true, 'true should pass');
  assertEqual(1 + 1, 2);
  assertDeepEqual({ b: [2, { c: true }], a: 'x' }, { a: 'x', b: [2, { c: true }] });
});

test('harness throw helpers', async () => {
  assertThrows(() => {
    throw new Error('sync failure');
  }, /sync/);
  await assertRejects(async () => {
    throw new Error('async failure');
  }, 'async');
});
