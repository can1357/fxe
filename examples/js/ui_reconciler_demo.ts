// Demonstrates fxe-ui: a static Panel of 1000 rectangles (cached after
// first build) and a Counter button (rebuilds only when state changes).
// Stats logged every 60 frames show cacheHits >> rebuilds.

import { App, type CommandBuffer, Primitives, Renderer, Window } from 'fxe';
// FXE resolves fxe-ui through the synthetic module loader.
import {
  Component,
  Draw,
  Layer,
  type Node,
  render,
  setRenderTarget,
  useEvent,
  useState,
} from 'fxe-ui';

const rgba = (r: number, g: number, b: number, a = 255): number =>
  ((r & 255) << 24) | ((g & 255) << 16) | ((b & 255) << 8) | (a & 255);

interface PanelProps {
  count: number;
}

const Panel = Component<PanelProps>(({ count }) => {
  const draws: Node[] = [];
  for (let i = 0; i < count; ++i) {
    const x = (i % 40) * 18 + 8;
    const y = Math.floor(i / 40) * 18 + 8;
    const color = rgba((i * 13) & 255, (i * 29) & 255, (i * 61) & 255, 255);
    draws.push(
      Draw(
        (cb: CommandBuffer) => {
          Primitives.fillRect(cb, x, y, 14, 14, 0, color);
        },
        [x, y, color],
      ),
    );
  }
  // deps: [count] — never changes during the demo, so this layer is cached
  // after the first frame.
  return Layer({ deps: [count], children: draws });
}, 'Panel');

interface CounterProps {
  win: Window;
  buttonRect: { x: number; y: number; w: number; h: number };
}

const Counter = Component<CounterProps>(({ win, buttonRect }) => {
  const [n, setN] = useState(0);

  useEvent(win, 'mousedown', (ev) => {
    const { x, y, w, h } = buttonRect;
    if (ev.x >= x && ev.x <= x + w && ev.y >= y && ev.y <= y + h) {
      setN((p) => p + 1);
    }
  });

  // Rebuilds whenever `n` changes; cached otherwise.
  return Layer({
    deps: [n, buttonRect.x, buttonRect.y, buttonRect.w, buttonRect.h],
    children: [
      Draw((cb) => {
        const { x, y, w, h } = buttonRect;
        Primitives.fillRect(cb, x, y, w, h, 0, rgba(40, 80, 160, 255));
        Primitives.drawText(
          cb,
          x + 12,
          y + h / 2 - 8,
          0,
          `clicks: ${n}`,
          18,
          rgba(255, 255, 255, 255),
        );
      }),
    ],
  });
}, 'Counter');

const win = new Window({ width: 900, height: 540, title: 'fxe-ui demo' });
const renderer = new Renderer(win);
setRenderTarget(win);

const buttonRect = { x: 720, y: 32, w: 140, h: 40 };

const root: Node = Layer({
  children: [Panel({ count: 1000 }), Counter({ win, buttonRect })],
});

let frameCount = 0;

App.run({ animate: false });

// Drive each frame: beginFrame() bumps the frame counter, then we render.
// Lazy mode redraws only when the window has a pending redraw (setState
// posts one via setRenderTarget).
win.run(
  (self) => {
    RenderStats.beginFrame();
    renderer.beginFrame();
    renderer.setClearColor(0.05, 0.06, 0.08, 1);
    render(root, renderer);
    renderer.endFrame();
    if (++frameCount % 60 === 0) {
      const s = RenderStats.snapshot();
      console.log(
        `[ui_reconciler_demo] frame=${s.frames} hits=${s.cacheHits} ` +
          `misses=${s.cacheMisses} rebuilds=${s.rebuilds} ` +
          `queueCalls=${s.queueCalls} verts=${s.verticesSubmitted}`,
      );
    }
    void self;
  },
  { animate: false },
);
