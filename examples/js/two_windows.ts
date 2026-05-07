// Two-window smoke. Type-checks against fxe.d.ts; not part of the runtime
// smoke suite (no GPU on CI). Demonstrates the multi-window event loop:
// every registered window's onFrame is driven from a single App.run.

import { App, Primitives, Renderer, Window } from 'fxe';

const w1 = new Window({ width: 320, height: 200, title: 'left', x: 80, y: 80 });
const w2 = new Window({ width: 320, height: 200, title: 'right', x: 440, y: 80 });
const r1 = new Renderer(w1);
const r2 = new Renderer(w2);

const draw = (r: Renderer, color: number): void => {
  r.beginFrame();
  Primitives.fillRect(r, 0, 0, 320, 200, 0, color);
  r.endFrame();
};

w1.on('close', () => {
  w2.close();
});
w2.on('close', () => {
  w1.close();
});

let frames = 0;
const onFrame = (): void => {
  draw(r1, 0xff3355ff);
  draw(r2, 0xff55ff33);
  if (++frames >= 30) {
    w1.close();
    w2.close();
  }
};

// First .run() drives the loop. Inside the callback we draw into both
// renderers because Window.run only lets us register a single onFrame per
// driver entry point. The loop snapshots App.windows() each iteration so the
// second window participates in input dispatch and close handling.
w1.run(onFrame, { animate: true, fps: 30 });
