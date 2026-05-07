// Smallest possible fxe demo: one triangle, repainted on demand.
//
// `win.run(draw)` blocks until the window is closed. The callback fires once
// at startup and then on every redraw request (resize / refresh / explicit
// requestRedraw). No busy loop.

import { Primitives, Renderer, type Vec4, Window } from 'fxe';

const win = new Window({ width: 640, height: 480, title: 'fxe — hello' });
const renderer = new Renderer(win);

const left: Vec4 = [-0.6, -0.5, 0, 1];
const right: Vec4 = [0.6, -0.5, 0, 1];
const top: Vec4 = [0.0, 0.6, 0, 1];

win.run(() => {
  renderer.beginFrame();
  Primitives.fillTriangle(renderer, left, right, top, 0x00ffffff);
  renderer.endFrame();
});
