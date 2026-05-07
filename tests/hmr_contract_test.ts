import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

const hmr = globalThis.__fxe_hmr;

function resetHmr(): void {
  for (const key of Object.keys(hmr.handlers)) {
    delete hmr.handlers[key];
  }
}

test('HMR registry is installed with callable accept and fire', () => {
  resetHmr();

  assert(hmr !== undefined, '__fxe_hmr must be installed');
  assertEqual(typeof hmr.accept, 'function');
  assertEqual(typeof hmr.fire, 'function');
  assertEqual(typeof hmr.handlers, 'object');

  const paths: string[] = [];
  hmr.accept('/module.ts', (path) => paths.push(path));

  assert(Array.isArray(hmr.handlers['/module.ts']), 'path handler bucket should be visible');
  assertEqual(hmr.fire('/module.ts'), 1);
  assertDeepEqual(paths, ['/module.ts']);
});

test('HMR fire only invokes handlers for the fired path', () => {
  resetHmr();

  const calls: string[] = [];
  hmr.accept('/hot.ts', () => calls.push('hot'));
  hmr.accept('/cold.ts', () => calls.push('cold'));

  assertEqual(hmr.fire('/hot.ts'), 1);
  assertDeepEqual(calls, ['hot']);
});

test('HMR function-only accept receives every fired path', () => {
  resetHmr();

  const calls: string[] = [];
  hmr.accept((path) => calls.push(path));

  assertEqual(hmr.fire('/any.ts'), 1);
  assertDeepEqual(calls, ['/any.ts']);
});

test('HMR fire returns zero when no handlers match', () => {
  resetHmr();

  assertEqual(hmr.fire('/missing.ts'), 0);
});

await run();
