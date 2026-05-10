import { assert, assertEqual, test } from './ts_harness.ts';

function importModule<T>(specifier: string): Promise<T> {
  // Dynamic import is the behavior under test; a static import cannot exercise HostImportModuleDynamically.
  return import(specifier) as Promise<T>;
}

type BoomModule = {
  boom(): void;
};

test('dynamic import preserves source map (TS .ts column mapping)', async () => {
  const mod = await importModule<BoomModule>('./fixtures/throws_at_line_4.ts');
  try {
    mod.boom();
    assert(false, 'expected boom() to throw');
  } catch (error) {
    const stack = error instanceof Error ? (error.stack ?? '') : String(error);
    assertEqual(/throws_at_line_4\.ts/.test(stack), true);
    assertEqual(/throws_at_line_4\.ts:4:/.test(stack), true);
  }
});
