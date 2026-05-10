import { readFileSync } from 'node:fs';

import { assert, assertEqual, assertRejects, assertThrows, test } from './ts_harness.ts';

const MISSING_IMAGE_PATH = './tests/__fxe_missing_image__.png';
const PNG_FIXTURE_PATH = './tests/golden/blur_box_shadow.png';
const CORRUPT_IMAGE_BYTES = new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7]);

type DisposableImageHandle = ImageHandle & { dispose(): void };

function loadFixtureBytes(): Uint8Array {
  return new Uint8Array(readFileSync(PNG_FIXTURE_PATH));
}

async function loadFixtureAsync(): Promise<DisposableImageHandle> {
  return (await Image.loadAsync(PNG_FIXTURE_PATH)) as DisposableImageHandle;
}

function burnCpu(iterations = 2_000_000): number {
  let acc = 0;
  for (let i = 0; i < iterations; ++i) {
    acc = (acc + ((i * 17) ^ (i >>> 3))) & 0xffff_ffff;
  }
  return acc;
}

test('Image.fromPixels accepts raw RGBA bytes', () => {
  const rgba = new Uint8Array([
    255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 255, 255, 255, 0, 12, 34, 56, 78, 90, 87, 65, 43,
  ]);

  const image = Image.fromPixels(rgba, 3, 2) as DisposableImageHandle;

  assertEqual(image.width(), 3);
  assertEqual(image.height(), 2);
  assertDeepBytes(image.bytes(), rgba);

  rgba[0] = 1;
  assertEqual(image.bytes()[0], 255, 'Image.fromPixels must copy caller-owned bytes');

  const copied = image.bytes();
  copied[1] = 99;
  assertEqual(image.bytes()[1], 0, 'ImageHandle.bytes must return a detached copy');
});

test('Image.fromPixels rejects raw byte length mismatches', () => {
  assertThrows(() => {
    Image.fromPixels(new Uint8Array([1, 2, 3]), 1, 1);
  }, /byte length mismatches/);
});

test('Image.decode decodes encoded bytes asynchronously', async () => {
  const encoded = loadFixtureBytes();
  const start = performance.now();
  const promise = Image.decode(encoded) as Promise<DisposableImageHandle>;
  const returnedMs = performance.now() - start;
  const loopStart = performance.now();
  const checksum = burnCpu();
  const loopMs = performance.now() - loopStart;
  const image = await promise;

  assert(checksum !== 0, 'tight loop should run after scheduling decode');
  assert(loopMs >= 0, 'tight loop timing must be observable');
  assert(
    returnedMs < 20,
    `Image.decode should return before decode completes (returned in ${returnedMs.toFixed(2)}ms)`,
  );
  assert(image.width() > 0);
  assert(image.height() > 0);
});

test('ImageHandle dispose is idempotent', () => {
  const image = Image.fromPixels(new Uint8Array([1, 2, 3, 4]), 1, 1) as DisposableImageHandle;

  image.dispose();
  image.dispose();

  assertEqual(image.width(), 0);
  assertEqual(image.height(), 0);
  const bytesAfterDispose: unknown = image.bytes();
  assertEqual(bytesAfterDispose, undefined);
});

test('Image.load throws for a missing file', () => {
  assert(!fs.existsSync(MISSING_IMAGE_PATH), 'missing-image test path unexpectedly exists');
  assertThrows(() => {
    Image.load(MISSING_IMAGE_PATH);
  }, /failed to read file/);
});

test('Image.loadAsync rejects for a missing file', async () => {
  assert(!fs.existsSync(MISSING_IMAGE_PATH), 'missing-image test path unexpectedly exists');
  await assertRejects(() => Image.loadAsync(MISSING_IMAGE_PATH), /read failed/);
});

test('Image.decode rejects corrupt encoded bytes', async () => {
  await assertRejects(() => Image.decode(CORRUPT_IMAGE_BYTES), /load_texture:/);
});

test('Image.loadAsync returns before decode finishes and parallel decodes overlap', async () => {
  const serialTimings: number[] = [];
  for (let i = 0; i < 3; ++i) {
    const startedAt = performance.now();
    const image = await loadFixtureAsync();
    serialTimings.push(performance.now() - startedAt);
    image.dispose();
  }
  const singleMs = Math.max(
    serialTimings.reduce((sum, value) => sum + value, 0) / serialTimings.length,
    0.001,
  );

  const callStartedAt = performance.now();
  const scheduled = Image.loadAsync(PNG_FIXTURE_PATH) as Promise<DisposableImageHandle>;
  const returnedMs = performance.now() - callStartedAt;
  const loopStartedAt = performance.now();
  const checksum = burnCpu();
  const loopMs = performance.now() - loopStartedAt;
  const scheduledImage = await scheduled;
  scheduledImage.dispose();

  assert(checksum !== 0, 'tight loop should complete while decode is in flight');
  assert(loopMs >= 0, 'tight loop timing must be measurable');
  assert(
    returnedMs < Math.max(singleMs * 0.5, 10),
    `Image.loadAsync blocked for ${returnedMs.toFixed(2)}ms; single decode average was ${singleMs.toFixed(2)}ms`,
  );

  const parallelStart = performance.now();
  const settled = await Promise.all(
    Array.from({ length: 3 }, () =>
      (Image.loadAsync(PNG_FIXTURE_PATH) as Promise<DisposableImageHandle>).then((image) => ({
        image,
        settledMs: performance.now() - parallelStart,
      })),
    ),
  );
  const parallelWallMs = performance.now() - parallelStart;
  const maxSettledMs = Math.max(...settled.map((entry) => entry.settledMs));
  for (const entry of settled) {
    entry.image.dispose();
  }

  assert(
    parallelWallMs <= singleMs * 2,
    `three parallel loads took ${parallelWallMs.toFixed(2)}ms; expected <= 2x single decode ${singleMs.toFixed(2)}ms`,
  );
  assert(
    maxSettledMs <= singleMs * 2,
    `parallel resolve times drifted to ${maxSettledMs.toFixed(2)}ms; expected <= 2x single decode ${singleMs.toFixed(2)}ms`,
  );
});

function assertDeepBytes(actual: Uint8Array, expected: Uint8Array): void {
  assertEqual(actual.byteLength, expected.byteLength, 'byte lengths differ');
  for (let i = 0; i < expected.byteLength; ++i) {
    assertEqual(actual[i], expected[i], `byte ${i} differs`);
  }
}
