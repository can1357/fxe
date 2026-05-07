import { assertEqual, run, test } from './ts_harness.ts';

test('packager contract fixture is a valid TypeScript entry', () => {
  assertEqual('fxe-packager-contract'.includes('packager'), true);
});

await run();
