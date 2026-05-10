import { assert, assertEqual, run, test } from './ts_harness.ts';

const Pr = Primitives as FXE.PrimitivesNamespace;
const WIDTH = 64;
const HEIGHT = 64;
const CLEAR = [0, 0, 0, 0] as const;
const SAMPLE_POINTS: Array<[number, number]> = [
  [8, 8],
  [24, 8],
  [40, 8],
  [56, 8],
  [8, 40],
  [24, 40],
  [40, 40],
  [56, 40],
];
const STRIPE_COLORS = [
  0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffff00ff, 0xff00ffff, 0x00ffffff, 0xff8800ff, 0xffffffff,
] as const;

function renderSeedFrame(renderer: FXE.OffscreenRenderer): void {
  renderer.beginFrame();
  for (let i = 0; i < STRIPE_COLORS.length; i += 1) {
    Pr.fillRect(renderer, i * 8, 0, 8, HEIGHT, 0, STRIPE_COLORS[i]);
  }
  renderer.endFrame();
}

function renderOverlayFrame(renderer: FXE.OffscreenRenderer): Uint8Array {
  renderer.beginFrame();
  Pr.fillRect(renderer, 0, 0, WIDTH, HEIGHT, 0, 0xffffff80);
  renderer.endFrame();
  return renderer.readPixels();
}

function sampleKey(pixels: Uint8Array, x: number, y: number): string {
  const at = (y * WIDTH + x) * 4;
  return `${pixels[at]},${pixels[at + 1]},${pixels[at + 2]},${pixels[at + 3]}`;
}

function uniqueSamples(pixels: Uint8Array): Set<string> {
  const out = new Set<string>();
  for (const [x, y] of SAMPLE_POINTS) out.add(sampleKey(pixels, x, y));
  return out;
}

function isClearImage(pixels: Uint8Array): boolean {
  for (let i = 0; i < pixels.length; i += 4) {
    if (
      pixels[i] !== CLEAR[0] ||
      pixels[i + 1] !== CLEAR[1] ||
      pixels[i + 2] !== CLEAR[2] ||
      pixels[i + 3] !== CLEAR[3]
    ) {
      return false;
    }
  }
  return true;
}

test('Renderer self backdrop blur toggles visible blur energy', () => {
  const base = new OffscreenRenderer({
    width: WIDTH,
    height: HEIGHT,
    multisample: 1,
    enableDepth: false,
  });
  const blurred = new OffscreenRenderer({
    width: WIDTH,
    height: HEIGHT,
    multisample: 1,
    enableDepth: false,
  });
  base.setClearColor(...CLEAR);
  blurred.setClearColor(...CLEAR);

  renderSeedFrame(base);
  renderSeedFrame(blurred);

  base.setSelfBackdropBlur(false);
  blurred.setSelfBackdropBlur(true, 24);

  const basePixels = renderOverlayFrame(base);
  const blurredPixels = renderOverlayFrame(blurred);

  if (
    process.versions.dawn === 'unknown' &&
    isClearImage(basePixels) &&
    isClearImage(blurredPixels)
  ) {
    return;
  }

  const baseSamples = uniqueSamples(basePixels);
  const blurredSamples = uniqueSamples(blurredPixels);

  assertEqual(
    baseSamples.size,
    1,
    'disabled self blur should leave a uniform translucent clear-color fill',
  );
  assert(
    blurredSamples.size > 1,
    'enabled self blur should preserve non-uniform blur energy from the prior frame',
  );

  let differsFromDisabled = false;
  for (const [x, y] of SAMPLE_POINTS) {
    if (sampleKey(basePixels, x, y) !== sampleKey(blurredPixels, x, y)) {
      differsFromDisabled = true;
      break;
    }
  }
  assert(differsFromDisabled, 'enabled self blur should change sampled framebuffer output');
});

await run();
