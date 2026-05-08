import { OffscreenRenderer, type Window, type WindowEventName } from 'fxe';
import { mount, View } from 'fxe-ui';

import { assert, run, test } from './ts_harness.ts';

test('boxShadow blur paints pixels outside the view bounds', () => {
  const width = 48;
  const height = 48;
  const renderer = new OffscreenRenderer({ width, height, multisample: 1, enableDepth: true });
  renderer.setClearColor(0, 0, 0, 0);
  const win = {
    framebufferSize: () => [width, height] as [number, number],
    requestRedraw: () => undefined,
    on: (_event: WindowEventName) => () => undefined,
  } as unknown as Window;

  const dispose = mount(
    View({
      style: { width, height, padding: 16 },
      children: View({
        style: {
          width: 16,
          height: 16,
          backgroundColor: 0xffffffff,
          shadowColor: 0x000000ff,
          shadowOffsetX: -6,
          shadowOffsetY: 0,
          shadowBlur: 6,
          shadowSpread: 2,
        },
      }),
    }),
    win,
    { renderer },
  );
  dispose();

  const pixels = renderer.readPixels();
  const alphaAt = (x: number, y: number): number => pixels[(y * width + x) * 4 + 3] ?? 0;
  let outsideAlpha = 0;
  for (let y = 16; y < 32; ++y) {
    for (let x = 8; x < 16; ++x) outsideAlpha += alphaAt(x, y);
  }
  assert(outsideAlpha > 0, `expected shadow alpha outside rect, got ${outsideAlpha}`);
});

await run();
