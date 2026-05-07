// Event-driven main loop. Demonstrates the canonical fxe pattern:
//
//   - Window stays open until the user closes it.
//   - The draw callback fires only when something visually changed
//     (resize / refresh / explicit requestRedraw). No busy spin.
//   - For animated scenes pass `{ animate: true }` (or `{ fps: <n> }`)
//     and the runtime paces the redraw at that frame rate.

import { Primitives, Renderer, Window } from 'fxe';

const win = new Window({
  width: 720,
  height: 420,
  title: 'fxe — event-driven loop',
});
const renderer = new Renderer(win);

let frames = 0;

// Lazy mode (default): repaints only when the window gets dirty.
//   - Resize the window: 1 redraw.
//   - Click on it / move it: 0 redraws.
//   - Close button: loop exits cleanly.
//
// Swap to animated mode by replacing `win.run(draw)` with
// `win.run(draw, { animate: true })` or `win.run(draw, { fps: 30 })`.
win.run((self) => {
  ++frames;
  const [w, h] = self.framebufferSize();

  renderer.beginFrame();
  Primitives.fillRect(renderer, 0, 0, w, h, 0.0, 0x111827ff);
  Primitives.fillRect(renderer, 40, 40, w - 80, 60, 0.1, 0x1f2937ff);
  Primitives.drawText(renderer, 56, 56, 0.0, `Frame ${frames}  —  ${w}x${h}`, 18, 0xe5e7ebff);
  Primitives.drawText(
    renderer,
    56,
    h - 80,
    0.0,
    'Resize the window to trigger a repaint. Close to exit.',
    14,
    0x9ca3afff,
  );
  renderer.endFrame();
});

console.log(`window closed after ${frames} frame(s).`);
