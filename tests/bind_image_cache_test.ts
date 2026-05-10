import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { assert, assertEqual, test } from './ts_harness.ts';

const PNG_FIXTURE_PATH = './tests/golden/blur_box_shadow.png';

type DisposableImageHandle = ImageHandle & { dispose(): void };

function resetCache(): ImageCacheStats {
  Image.cacheClear();
  Image.setCacheBudget(256 * 1024 * 1024);
  return Image.cacheStats();
}

test('Image cache tracks hits, count, eviction, and clear', () => {
  const tmp = join(
    tmpdir(),
    `fxe-image-cache-${Date.now()}-${Math.random().toString(16).slice(2)}`,
  );
  mkdirSync(tmp, { recursive: true });
  const secondPath = join(tmp, 'copy-b.png');
  const thirdPath = join(tmp, 'copy-c.png');
  writeFileSync(secondPath, readFileSync(PNG_FIXTURE_PATH));
  writeFileSync(thirdPath, readFileSync(PNG_FIXTURE_PATH));

  const originalBudget = Image.cacheStats().budget;
  try {
    resetCache();

    const first = Image.load(PNG_FIXTURE_PATH) as DisposableImageHandle;
    const afterFirst = Image.cacheStats();
    assertEqual(afterFirst.count, 1);
    assertEqual(afterFirst.hits, 0);
    assert(afterFirst.misses >= 1, 'first load should miss the cache');

    const second = Image.load(PNG_FIXTURE_PATH) as DisposableImageHandle;
    const afterSecond = Image.cacheStats();
    assertEqual(afterSecond.count, 1);
    assert(afterSecond.hits >= 1, 'second load should hit the cache');

    const bytesPerImage = first.width() * first.height() * 4;
    assert(bytesPerImage > 0, 'fixture image must decode to at least one pixel');

    const third = Image.load(secondPath) as DisposableImageHandle;
    const fourth = Image.load(thirdPath) as DisposableImageHandle;
    const afterThree = Image.cacheStats();
    assertEqual(afterThree.count, 3);
    assertEqual(afterThree.bytes, bytesPerImage * 3);

    Image.setCacheBudget(bytesPerImage + 1);
    const afterEviction = Image.cacheStats();
    assertEqual(afterEviction.budget, bytesPerImage + 1);
    assertEqual(afterEviction.count, 1);
    assert(afterEviction.bytes <= afterEviction.budget, 'cache bytes must respect the budget');

    const removed = Image.cacheEvict(thirdPath);
    assertEqual(removed, 1);
    assertEqual(Image.cacheStats().count, 0);

    Image.cacheClear();
    const afterClear = Image.cacheStats();
    assertEqual(afterClear.count, 0);
    assertEqual(afterClear.bytes, 0);

    first.dispose();
    second.dispose();
    third.dispose();
    fourth.dispose();
  } finally {
    Image.cacheClear();
    Image.setCacheBudget(originalBudget);
    rmSync(tmp, { recursive: true, force: true });
  }
});
