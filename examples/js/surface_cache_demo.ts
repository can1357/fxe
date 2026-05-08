// Surface caching demo — bake a complex subtree into an offscreen texture
// once, then on subsequent frames sample the texture as a single quad
// instead of re-emitting all primitives.
//
// Layout: window split in half. Left side draws a heavy text-and-shapes
// composition every frame. Right side bakes the same composition into a
// 200×200 offscreen surface ONCE at startup, then draws a textured quad
// sampling that surface every frame.
//
// Visually identical; the right side is much cheaper per frame.

import { App, OffscreenRenderer, Primitives, Renderer, Window } from 'fxe';

const W = 600;
const H = 240;
const SURFACE_W = 240;
const SURFACE_H = 200;

const win = new Window({ width: W, height: H, title: 'surface cache demo' });
const renderer = new Renderer(win);

// Bake into an offscreen target once at startup.
const off = new OffscreenRenderer({ width: SURFACE_W, height: SURFACE_H, parent: renderer });

function paintSubtree(target: Renderer | OffscreenRenderer): void {
  // Big rounded background.
  Primitives.fillRect(target, 0, 0, SURFACE_W, SURFACE_H, 0, 0x1e293bff);
  // Title bar.
  Primitives.fillRect(target, 0, 0, SURFACE_W, 40, 0, 0xfbbf24ff);
  Primitives.drawText(target, 12, 10, 0, 'Surface Cache', 18, 0x0f172aff);
  // A grid of cells.
  for (let i = 0; i < 6; ++i) {
    for (let j = 0; j < 4; ++j) {
      const x = 12 + i * 36;
      const y = 60 + j * 30;
      Primitives.fillRect(
        target,
        x,
        y,
        32,
        24,
        0,
        0x334155ff + ((i * 30 + j * 8) & 0x3f),
      );
      Primitives.drawText(target, x + 4, y + 4, 0, `${i},${j}`, 12, 0xe2e8f0ff);
    }
  }
}

// One-shot bake.
off.beginFrame();
off.setClearColor(0, 0, 0, 0);
paintSubtree(off);
off.endFrame();

// Bind the baked color attachment to user-texture slot 0.
renderer.bindUserTexture(0, off);

let frame = 0;
win.setFrameCallback(() => {
  renderer.beginFrame();
  renderer.setClearColor(0.06, 0.07, 0.1, 1);

  // Left half: paint the subtree from scratch every frame (the slow path).
  paintSubtree(renderer);

  // Right half: draw a textured quad sampling the baked surface.
  Primitives.drawTextureQuad(renderer, 0, SURFACE_W + 60, 20, SURFACE_W, SURFACE_H);

  // Frame counter to confirm we're rendering.
  Primitives.drawText(
    renderer,
    8,
    H - 20,
    0,
    `frame ${frame++} | left = redraw, right = surface-cache`,
    12,
    0xfbbf24ff,
  );

  renderer.endFrame();
});

win.requestRedraw();
App.run({ animate: true });
