import vm, {
  compileFunction,
  createContext,
  isContext,
  runInContext,
  runInNewContext,
  Script,
} from 'node:vm';

import { assert, assertEqual, run, test } from './ts_harness.ts';

test('node:vm runInNewContext evaluates against sandbox values', () => {
  const r = runInNewContext('a + b', { a: 2, b: 3 });
  assertEqual(r, 5);
  assertEqual(vm.runInNewContext('a + b', { a: 4, b: 6 }), 10);
});

test('node:vm createContext propagates global mutations', () => {
  const sandbox = createContext({ x: 1 });
  assert(isContext(sandbox), 'createContext must return a contextified object');
  runInContext('x = x + 1', sandbox);
  assertEqual(sandbox.x, 2);
});

test('node:vm Script reuses compiled code across contexts', () => {
  const script = new Script('x = x + 2; x');
  const sandbox = createContext({ x: 3 });
  assertEqual(script.runInContext(sandbox), 5);
  assertEqual(sandbox.x, 5);
});

test('node:vm compileFunction returns callable functions', () => {
  const fn = compileFunction('return a + b;', ['a', 'b']);
  assertEqual(fn(7, 8), 15);
});

await run();
