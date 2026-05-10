import { mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { tmpdir } from 'node:os';

import { assert, assertEqual, test } from './ts_harness.ts';

const GIF_BASE64 =
  'R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQICgAAACwAAAAAAgACAAAIBgABCAQQEAAh+QQIDAAAACwAAAAAAgACAIEA/wAAAAAAAAAAAAAIBgABCAQQEAA7';
const APNG_BASE64 =
  'iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAACGFjVEwAAAACAAAAAPONk3AAAAAaZmNUTAAAAAAAAAACAAAAAgAAAAAAAAAAAAEACgAA6FTcAAAAABVJREFUeJxj/M/A8J+BgYGBCUSAMAAfFwICAkezFAAAABpmY1RMAAAAAQAAAAIAAAACAAAAAAAAAAAAAwAZAAAXh3idAAAAGWZkQVQAAAACeJxjZPjP8J+BgYGBCUSAMAAeGAICRb04jgAAAABJRU5ErkJggg==';
const LOTTIE_JSON = JSON.stringify({ v: '5.7.0', fr: 60, ip: 0, op: 1, w: 2, h: 2, layers: [] });

type DisposableImageHandle = ImageHandle & { dispose(): void };
type DisposableAnimatedImageHandle = AnimatedImageHandle & { dispose(): void };

function writeFixture(name: string, bytes: Uint8Array | string): string {
  const dir = join(tmpdir(), `fxe-image-anim-${Date.now()}-${Math.random().toString(16).slice(2)}`);
  mkdirSync(dir, { recursive: true });
  const path = join(dir, name);
  writeFileSync(path, bytes);
  return path;
}

function pixelSignature(image: ImageHandle): string {
  return Array.from(image.bytes().slice(0, 4)).join(',');
}

function assertFrameImage(
  frame: { delayMs: number; image: ImageHandle },
  expectedDelayMs: number,
  width: number,
): void {
  assertEqual(frame.delayMs, expectedDelayMs);
  assertEqual(frame.image.width(), width);
  assertEqual(frame.image.height(), width);
}

test('Image.loadAnimated decodes 2-frame GIFs', async () => {
  const path = writeFixture('two-frame.gif', Buffer.from(GIF_BASE64, 'base64'));
  try {
    const animated = (await Image.loadAnimated(path)) as DisposableAnimatedImageHandle;
    assertEqual(animated.frameCount, 2);
    assertEqual(animated.durationMs, 220);
    assertEqual(animated.frames.length, 2);
    assertFrameImage(animated.frames[0], 100, 2);
    assertFrameImage(animated.frames[1], 120, 2);

    const first = animated.frame(0) as DisposableImageHandle;
    const second = animated.frame(animated.frames[0].delayMs) as DisposableImageHandle;
    try {
      assertEqual(first.width(), 2);
      assertEqual(first.height(), 2);
      assertEqual(second.width(), 2);
      assertEqual(second.height(), 2);
      assert(
        pixelSignature(first) !== pixelSignature(second),
        'GIF frames should differ by sampled pixel',
      );
    } finally {
      first.dispose();
      second.dispose();
    }

    animated.dispose();
  } finally {
    rmSync(dirname(path), { recursive: true, force: true });
  }
});

test('Image.loadAnimated decodes 2-frame APNGs', async () => {
  const path = writeFixture('two-frame.png', Buffer.from(APNG_BASE64, 'base64'));
  try {
    const animated = (await Image.loadAnimated(path)) as DisposableAnimatedImageHandle;
    assertEqual(animated.frameCount, 2);
    assertEqual(animated.durationMs, 220);
    assertEqual(animated.frames.length, 2);
    assertFrameImage(animated.frames[0], 100, 2);
    assertFrameImage(animated.frames[1], 120, 2);

    const first = animated.frame(0) as DisposableImageHandle;
    const second = animated.frame(animated.frames[0].delayMs) as DisposableImageHandle;
    try {
      assertEqual(first.width(), 2);
      assertEqual(first.height(), 2);
      assertEqual(second.width(), 2);
      assertEqual(second.height(), 2);
      assert(
        pixelSignature(first) !== pixelSignature(second),
        'APNG frames should differ by sampled pixel',
      );
    } finally {
      first.dispose();
      second.dispose();
    }

    animated.dispose();
  } finally {
    rmSync(dirname(path), { recursive: true, force: true });
  }
});

test('Image.loadLottie returns a 1-frame placeholder', async () => {
  const path = writeFixture('placeholder.json', LOTTIE_JSON);
  try {
    const animated = (await Image.loadLottie(path)) as DisposableAnimatedImageHandle;
    assertEqual(animated.frameCount, 1);
    assertEqual(animated.durationMs, 0);
    assertEqual(animated.frames.length, 1);
    assertFrameImage(animated.frames[0], 0, 1);

    const frame = animated.frame(0) as DisposableImageHandle;
    try {
      assertEqual(frame.width(), 1);
      assertEqual(frame.height(), 1);
      assertEqual(pixelSignature(frame), '0,0,0,0');
    } finally {
      frame.dispose();
    }

    animated.dispose();
  } finally {
    rmSync(dirname(path), { recursive: true, force: true });
  }
});
