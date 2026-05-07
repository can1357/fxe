import { assert, assertEqual, assertRejects, assertThrows, test } from './ts_harness.ts';

const MISSING_IMAGE_PATH = './tests/__fxe_missing_image__.png';

type DisposableImageHandle = ImageHandle & { dispose(): void };

test('Image.fromBytes accepts raw RGBA bytes', () => {
  const rgba = new Uint8Array([
    255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 255, 255, 255, 0, 12, 34, 56, 78, 90, 87, 65, 43,
  ]);

  const image = Image.fromBytes(rgba, 3, 2) as DisposableImageHandle;

  assertEqual(image.width(), 3);
  assertEqual(image.height(), 2);
  assertDeepBytes(image.bytes(), rgba);

  rgba[0] = 1;
  assertEqual(image.bytes()[0], 255, 'Image.fromBytes must copy caller-owned bytes');

  const copied = image.bytes();
  copied[1] = 99;
  assertEqual(image.bytes()[1], 0, 'ImageHandle.bytes must return a detached copy');
});

test('Image.fromBytes rejects raw byte length mismatches', () => {
  assertThrows(() => {
    Image.fromBytes(new Uint8Array([1, 2, 3]), 1, 1);
  }, /byte length mismatches/);
});

test('ImageHandle dispose is idempotent', () => {
  const image = Image.fromBytes(new Uint8Array([1, 2, 3, 4]), 1, 1) as DisposableImageHandle;

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

function assertDeepBytes(actual: Uint8Array, expected: Uint8Array): void {
  assertEqual(actual.byteLength, expected.byteLength, 'byte lengths differ');
  for (let i = 0; i < expected.byteLength; ++i) {
    assertEqual(actual[i], expected[i], `byte ${i} differs`);
  }
}
