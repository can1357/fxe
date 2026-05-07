import { Primitives, Renderer, Window } from 'fxe';

const W = 520;
const H = 320;
const TITLEBAR_H = 36;
const BUTTON = 28;
const GAP = 8;

const rgba = (r: number, g: number, b: number, a = 255): number =>
  ((r & 255) << 24) | ((g & 255) << 16) | ((b & 255) << 8) | (a & 255);

const win = new Window({
  width: W,
  height: H,
  title: 'custom titlebar',
  decorated: false,
  resizable: true,
});

win.setTitleBarStyle('customButtons');
win.setWindowControlsOverlay(true);
win.setTrafficLightPosition(12, 10);

let closeRect = { x: W - BUTTON - GAP, y: 4, width: BUTTON, height: BUTTON };
let minRect = { x: closeRect.x - BUTTON - GAP, y: 4, width: BUTTON, height: BUTTON };
let dragWidth = minRect.x - GAP;

const layoutChrome = (width: number): void => {
  closeRect = { x: width - BUTTON - GAP, y: 4, width: BUTTON, height: BUTTON };
  minRect = { x: closeRect.x - BUTTON - GAP, y: 4, width: BUTTON, height: BUTTON };
  dragWidth = Math.max(0, minRect.x - GAP);
  // Leave the JS button area out of the drag region so clicks reach handlers.
  win.setDragRegion([{ x: 0, y: 0, width: dragWidth, height: TITLEBAR_H }]);
};

layoutChrome(W);

const renderer = new Renderer(win);
renderer.setClearColor(24 / 255, 28 / 255, 36 / 255, 1);

let mouseX = -1;
let mouseY = -1;

const inside = (r: typeof closeRect, x: number, y: number): boolean =>
  x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;

win.on('mousemove', (ev) => {
  mouseX = ev.x;
  mouseY = ev.y;
});

win.on('mousedown', (ev) => {
  if (ev.button !== 0) return;
  if (inside(closeRect, ev.x, ev.y)) {
    win.close();
  } else if (inside(minRect, ev.x, ev.y)) {
    win.minimize();
  }
});

win.run(
  (self) => {
    const [w, h] = self.framebufferSize();
    if (dragWidth !== Math.max(0, w - BUTTON * 2 - GAP * 3)) layoutChrome(w);

    renderer.beginFrame();
    Primitives.fillRect(renderer, 0, 0, w, h, 0.08, rgba(24, 28, 36));
    Primitives.fillRect(renderer, 0, 0, w, TITLEBAR_H, 0.05, rgba(15, 23, 42));
    Primitives.drawText(renderer, 14, 11, 0.03, 'FXE custom titlebar', 13, rgba(226, 232, 240));

    const minHover = inside(minRect, mouseX, mouseY);
    const closeHover = inside(closeRect, mouseX, mouseY);
    Primitives.fillRect(
      renderer,
      minRect.x,
      minRect.y,
      minRect.width,
      minRect.height,
      0.03,
      minHover ? rgba(51, 65, 85) : rgba(30, 41, 59),
    );
    Primitives.fillRect(
      renderer,
      closeRect.x,
      closeRect.y,
      closeRect.width,
      closeRect.height,
      0.03,
      closeHover ? rgba(185, 28, 28) : rgba(127, 29, 29),
    );
    Primitives.drawText(renderer, minRect.x + 9, minRect.y + 8, 0.02, '–', 15, rgba(226, 232, 240));
    Primitives.drawText(
      renderer,
      closeRect.x + 9,
      closeRect.y + 8,
      0.02,
      '×',
      15,
      rgba(255, 241, 242),
    );

    Primitives.drawText(
      renderer,
      32,
      92,
      0.03,
      'Drag the titlebar strip. Use the JS buttons at top right.',
      14,
      rgba(203, 213, 225),
    );
    renderer.endFrame();
  },
  { fps: 60 },
);
