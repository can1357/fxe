// Sprite demo: build a 64x64 RGBA checkerboard in JS, register it in a
// Spritesheet, and draw it every frame for 30 frames before closing.
//
// NOTE: drawSprite is currently a v0 — it emits a tinted rect of the
// requested size and ignores the source pixels (the renderer only exposes
// a single global atlas today). The Spritesheet/Image plumbing is real,
// so this script doubles as a smoke test for those bindings.

import { Image, Primitives, Renderer, Spritesheet, Window } from 'fxe';

const W = 64;
const H = 64;
const buf = new Uint8Array(W * H * 4);
for (let y = 0; y < H; ++y) {
  for (let x = 0; x < W; ++x) {
    const i = (y * W + x) * 4;
    const checker = ((x >> 3) ^ (y >> 3)) & 1;
    buf[i + 0] = checker ? 0xff : 0x20;
    buf[i + 1] = checker ? 0xa0 : 0x20;
    buf[i + 2] = checker ? 0x20 : 0xff;
    buf[i + 3] = 0xff;
  }
}

const img = Image.fromPixels(buf, W, H);
const sheet = new Spritesheet();
const spriteId = sheet.add(img);
const resolved = sheet.resolve(spriteId);
console.log(
  `sprite ${spriteId} resolved -> tex=${resolved.textureId} ` +
    `uv=[${resolved.u0},${resolved.v0}..${resolved.u1},${resolved.v1}] ` +
    `size=${resolved.width}x${resolved.height}`,
);

// Image.dispose() releases the JS handle's pixel ref; the Spritesheet
// retains a shared_ptr internally, so the sheet stays valid.
img.dispose();

const win = new Window({ width: 480, height: 480, title: 'fxe — sprite demo' });
const renderer = new Renderer(win);

const FRAMES = 30;
let frame = 0;
win.run(
  (self) => {
    const [fbw, fbh] = self.framebufferSize();
    renderer.beginFrame();
    Primitives.fillRect(renderer, 0, 0, fbw, fbh, 0.0, 0x101820ff);

    // Walk the sprite across the screen so the v0 tinted-rect path is
    // visibly animated; replace tint per-frame so each frame is distinct.
    const t = frame / FRAMES;
    const x = 32 + t * (fbw - 96 - 32);
    const y = fbh / 2 - 48;
    const tint = 0xff8040ff;
    Primitives.drawSprite(renderer, spriteId, x, y, 96, 96, 0.1, tint);

    Primitives.drawText(renderer, 16, 16, 0.0, `frame ${frame + 1} / ${FRAMES}`, 14, 0xe5e7ebff);
    renderer.endFrame();

    if (++frame >= FRAMES) {
      sheet.dispose();
      self.close();
    }
  },
  { fps: 30 },
);
