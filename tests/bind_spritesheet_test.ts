import { assertEqual, assertThrows, test } from './ts_harness.ts';

function rgba(width: number, height: number, seed: number): Uint8Array {
  const out = new Uint8Array(width * height * 4);
  for (let i = 0; i < width * height; i += 1) {
    const j = i * 4;
    out[j] = (seed + i) & 0xff;
    out[j + 1] = (seed + i * 3) & 0xff;
    out[j + 2] = (seed + i * 5) & 0xff;
    out[j + 3] = 255;
  }
  return out;
}

function image(width: number, height: number, seed = 0): ImageHandle {
  return Image.fromBytes(rgba(width, height, seed), width, height);
}

test('Spritesheet.add resolves a full image sprite', () => {
  const sheet = new Spritesheet();
  const spriteId = sheet.add(image(2, 3, 11));

  assertEqual(spriteId, 1);
  const resolved = sheet.resolve(spriteId, 0);
  assertEqual(resolved.textureId, 1);
  assertEqual(resolved.u0, 0);
  assertEqual(resolved.v0, 0);
  assertEqual(resolved.u1, 1);
  assertEqual(resolved.v1, 1);
  assertEqual(resolved.width, 2);
  assertEqual(resolved.height, 3);
});

test('Spritesheet.add resolves an explicit image rect', () => {
  const sheet = new Spritesheet();
  const spriteId = sheet.add(image(4, 2, 23), [1, 1, 2, 1]);

  assertEqual(spriteId, 1);
  const resolved = sheet.resolve(spriteId);
  assertEqual(resolved.textureId, 1);
  assertEqual(resolved.u0, 0.25);
  assertEqual(resolved.v0, 0.5);
  assertEqual(resolved.u1, 0.75);
  assertEqual(resolved.v1, 1);
  assertEqual(resolved.width, 2);
  assertEqual(resolved.height, 1);
});

test('Spritesheet.addAnimated resolves frame texture ids by time', () => {
  const sheet = new Spritesheet();
  const spriteId = sheet.addAnimated([image(1, 1, 31), image(1, 1, 41)], [100, 150]);

  assertEqual(spriteId, 0x80001);
  assertEqual(sheet.resolve(spriteId, 0).textureId, 1);
  assertEqual(sheet.resolve(spriteId, 100).textureId, 1);
  assertEqual(sheet.resolve(spriteId, 101).textureId, 2);
  assertEqual(sheet.resolve(spriteId, 249).textureId, 2);
  assertEqual(sheet.resolve(spriteId, 251).textureId, 1);
});

test('Spritesheet retains added image data after source disposal', () => {
  const sheet = new Spritesheet();
  const source = image(3, 1, 51);
  const spriteId = sheet.add(source);
  source.dispose();

  const resolved = sheet.resolve(spriteId);
  assertEqual(resolved.textureId, 1);
  assertEqual(resolved.width, 3);
  assertEqual(resolved.height, 1);
});

test('Spritesheet.dispose clears sprites and permits reuse', () => {
  const sheet = new Spritesheet();
  const spriteId = sheet.add(image(2, 2, 61));
  sheet.dispose();

  const disposed = sheet.resolve(spriteId);
  assertEqual(disposed.textureId, spriteId);
  assertEqual(disposed.width, 0);
  assertEqual(disposed.height, 0);
  assertEqual(disposed.u0, 0);
  assertEqual(disposed.v0, 0);
  assertEqual(disposed.u1, 1);
  assertEqual(disposed.v1, 1);

  const nextId = sheet.add(image(1, 2, 71));
  assertEqual(nextId, 1);
  const next = sheet.resolve(nextId);
  assertEqual(next.textureId, 1);
  assertEqual(next.width, 1);
  assertEqual(next.height, 2);
});

test('Spritesheet handles invalid sprite ids and rejects invalid inputs', () => {
  const sheet = new Spritesheet();

  const missing = sheet.resolve(12345);
  assertEqual(missing.textureId, 12345);
  assertEqual(missing.width, 0);
  assertEqual(missing.height, 0);

  const badAnimated = sheet.resolve(0x80002, 10);
  assertEqual(badAnimated.textureId, 0);
  assertEqual(badAnimated.width, 0);
  assertEqual(badAnimated.height, 0);

  assertThrows(() => sheet.add(undefined as unknown as ImageHandle), /live ImageHandle/);
  assertThrows(
    () => sheet.add(image(1, 1), [0, 0, 1] as unknown as [number, number, number, number]),
    /rect/,
  );
  assertThrows(() => sheet.addAnimated([], []), /empty image list/);
  assertThrows(
    () => sheet.addAnimated([image(1, 1), undefined as unknown as ImageHandle], [10, 10]),
    /live ImageHandle/,
  );
});
