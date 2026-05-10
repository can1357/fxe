import { assertDeepEqual, assertRejects, test } from './ts_harness.ts';

function importWithAttributes<T>(specifier: string, options?: object): Promise<T> {
  // Dynamic import is the behavior under test; a static import cannot exercise host import attributes.
  return import(specifier, options) as Promise<T>;
}

test('JSON module accepts type=json attribute', async () => {
  const mod = await importWithAttributes<{ default: { value: number } }>('./fixtures/data.json', {
    with: { type: 'json' },
  });
  assertDeepEqual(mod.default, { value: 42 });
});

test('JSON module rejects non-json type', async () => {
  await assertRejects(
    () => importWithAttributes('./fixtures/data.json', { with: { type: 'css' } }),
    /mismatched type/i,
  );
});

test('JS module rejects type=json', async () => {
  await assertRejects(
    () => importWithAttributes('./fixtures/dummy.ts', { with: { type: 'json' } }),
    /Non-JSON module/i,
  );
});
