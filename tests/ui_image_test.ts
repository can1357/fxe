import { CommandBuffer, type ImageHandle } from 'fxe';
import { render, Image as UIImage } from 'fxe-ui';

import { AnimatedValue } from '../packages/fxe-ui/src/animated/timing.ts';
import { tickFrame } from '../packages/fxe-ui/src/reconciler/frame_loop.ts';
import {
  DEFAULT_IMAGE_PLACEHOLDER_COLOR,
  resolveImageContentRect,
  resolveImagePlaceholderColor,
  startImageFadeAnimation,
} from '../packages/fxe-ui/src/components/Image.ts';
import { assert, assertDeepEqual, assertEqual, run, test } from './ts_harness.ts';

type MutableImageNamespace = {
  fromPixels(rgba: Uint8Array, width: number, height: number): ImageHandle;
  loadAsync: (path: string) => Promise<ImageHandle>;
};

const NativeImageApi = (globalThis as unknown as { Image: MutableImageNamespace }).Image;

function rgba(width: number, height: number, seed = 0): Uint8Array {
  const out = new Uint8Array(width * height * 4);
  for (let i = 0; i < out.length; i += 4) {
    out[i] = (seed + i) & 0xff;
    out[i + 1] = (seed + i * 3) & 0xff;
    out[i + 2] = (seed + i * 7) & 0xff;
    out[i + 3] = 0xff;
  }
  return out;
}

function makeImage(width: number, height: number, seed = 0): ImageHandle {
  return NativeImageApi.fromPixels(rgba(width, height, seed), width, height);
}

function assertNear(actual: number, expected: number, epsilon = 1e-3): void {
  assert(
    Math.abs(actual - expected) <= epsilon,
    `expected ${actual} to be within ${epsilon} of ${expected}`,
  );
}

test('Image enters loading state for async string sources before decode resolves', async () => {
  const mutableImage = NativeImageApi;
  const originalLoadAsync = mutableImage.loadAsync;
  const pending = Promise.withResolvers<ImageHandle>();
  const requested: string[] = [];
  const loads: Array<[number, number]> = [];
  mutableImage.loadAsync = (path: string) => {
    requested.push(path);
    return pending.promise;
  };
  try {
    render(
      UIImage({
        key: 'ui-image-pending-explicit',
        source: 'explicit.png',
        placeholder: { color: 0x112233ff },
        style: { width: 24, height: 16 },
        onLoad: (width, height) => loads.push([width, height]),
      }),
      new CommandBuffer(),
    );
    render(
      UIImage({
        key: 'ui-image-pending-default',
        source: 'fallback.png',
        style: { width: 12, height: 8 },
        onLoad: (width, height) => loads.push([width, height]),
      }),
      new CommandBuffer(),
    );
    assertDeepEqual(requested, ['explicit.png', 'fallback.png']);
    assertDeepEqual(loads, []);
  } finally {
    mutableImage.loadAsync = originalLoadAsync;
  }
});

test('Image calls onLoad after async decode and onError on decode failure', async () => {
  const mutableImage = NativeImageApi;
  const originalLoadAsync = mutableImage.loadAsync;
  const first = Promise.withResolvers<ImageHandle>();
  const loads: Array<[number, number]> = [];
  const errors: string[] = [];
  let calls = 0;
  mutableImage.loadAsync = (path: string) => {
    calls += 1;
    if (path === 'broken.png') return Promise.reject(new Error('decode failed'));
    return first.promise;
  };
  try {
    render(
      UIImage({
        key: 'ui-image-callbacks-load',
        source: 'loaded.png',
        style: { width: 18, height: 12 },
        onLoad: (width, height) => loads.push([width, height]),
      }),
      new CommandBuffer(),
    );
    first.resolve(makeImage(5, 4, 11));
    await Promise.resolve();
    await Promise.resolve();
    render(
      UIImage({
        key: 'ui-image-callbacks-load',
        source: 'loaded.png',
        style: { width: 18, height: 12 },
        onLoad: (width, height) => loads.push([width, height]),
      }),
      new CommandBuffer(),
    );
    assertDeepEqual(loads, [[5, 4]]);

    render(
      UIImage({
        key: 'ui-image-callbacks-error',
        source: 'broken.png',
        style: { width: 18, height: 12 },
        onError: (error) => errors.push(error.message),
      }),
      new CommandBuffer(),
    );
    await Promise.resolve();
    await Promise.resolve();
    render(
      UIImage({
        key: 'ui-image-callbacks-error',
        source: 'broken.png',
        style: { width: 18, height: 12 },
        onError: (error) => errors.push(error.message),
      }),
      new CommandBuffer(),
    );
    assertEqual(calls, 2);
    assertDeepEqual(errors, ['decode failed']);
  } finally {
    mutableImage.loadAsync = originalLoadAsync;
  }
});

test('ImageHandle source skips async loading and uses intrinsic size when style does not override it', () => {
  const mutableImage = NativeImageApi;
  const originalLoadAsync = mutableImage.loadAsync;
  const handle = makeImage(3, 2, 21);
  const loads: Array<[number, number]> = [];
  let loadAsyncCalls = 0;
  mutableImage.loadAsync = async () => {
    loadAsyncCalls += 1;
    return makeImage(1, 1, 99);
  };
  try {
    const cb = new CommandBuffer();
    render(
      UIImage({
        key: 'ui-image-direct-handle',
        source: handle,
        onLoad: (width, height) => loads.push([width, height]),
      }),
      cb,
    );
    assertEqual(loadAsyncCalls, 0);
    assertDeepEqual(loads, [[3, 2]]);
    assert(cb.vertexCount() > 0, 'direct handle path should render immediately');
  } finally {
    mutableImage.loadAsync = originalLoadAsync;
    handle.dispose();
  }
});

test('Image fade timeline animates opacity from 0 to 1 with Animated.timing', () => {
  const value = new AnimatedValue(0);
  const animation = startImageFadeAnimation(value, 180);
  tickFrame(90);
  const midway = value.getValue();
  assert(midway > 0.5 && midway < 1, `expected midway opacity in (0.5, 1), got ${midway}`);
  tickFrame(90);
  assertNear(value.getValue(), 1);
  animation.stop();
});

test('Image resize modes compute the expected destination rect', () => {
  const box = { x: 10, y: 20, width: 100, height: 60 };
  assertDeepEqual(resolveImageContentRect(box, 50, 50, 'stretch'), box);
  assertDeepEqual(resolveImageContentRect(box, 50, 50, 'center'), {
    x: 35,
    y: 25,
    width: 50,
    height: 50,
  });
  assertDeepEqual(resolveImageContentRect(box, 50, 100, 'contain'), {
    x: 45,
    y: 20,
    width: 30,
    height: 60,
  });
  assertDeepEqual(resolveImageContentRect(box, 100, 50, 'cover'), {
    x: 0,
    y: 20,
    width: 120,
    height: 60,
  });
});

test('Image placeholder color helper resolves both explicit and implicit color fallbacks', () => {
  const preview = makeImage(1, 1, 7);
  try {
    assertEqual(resolveImagePlaceholderColor(undefined), DEFAULT_IMAGE_PLACEHOLDER_COLOR);
    assertEqual(resolveImagePlaceholderColor('color'), DEFAULT_IMAGE_PLACEHOLDER_COLOR);
    assertEqual(resolveImagePlaceholderColor({ color: 0xaabbccdd }), 0xaabbccdd);
    assertEqual(resolveImagePlaceholderColor(preview), undefined);
  } finally {
    preview.dispose();
  }
});

await run();
