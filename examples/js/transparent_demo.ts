import { Primitives, Renderer, Window } from 'fxe';

const rgba = (r: number, g: number, b: number, a = 255): number =>
  ((r & 255) << 24) | ((g & 255) << 16) | ((b & 255) << 8) | (a & 255);

// Premultiplied alpha pipeline: pre-multiply RGB by A so the surface composites
// correctly under CompositeAlphaMode::Premultiplied.
const rgbaPm = (r: number, g: number, b: number, a: number): number => {
  const af = (a & 255) / 255;
  return rgba(
    Math.round((r & 255) * af),
    Math.round((g & 255) * af),
    Math.round((b & 255) * af),
    a & 255,
  );
};

const W = 400;
const H = 200;

const win = new Window({
  width: W,
  height: H,
  title: 'transparent',
  transparent: true,
  decorated: false,
});

// 32px-tall drag strip across the top.
win.setDragRegion([{ x: 0, y: 0, width: W, height: 32 }]);

const renderer = new Renderer(win);
renderer.setClearColor(0, 0, 0, 0);
win.setBlurBehind(true);
win.setVibrancy('sidebar');

let frame = 0;

win.run(
  (self) => {
    const [w, h] = self.framebufferSize();

    renderer.beginFrame();

    // Solid rounded panel in the middle of the otherwise transparent window.
    // fillRectRounded takes a model matrix mapping a unit cube into screen space.
    const cx = w * 0.5;
    const cy = h * 0.5;
    const rw = w - 32;
    const rh = h - 48;
    const m = new Float32Array([rw * 0.5, 0, 0, 0, 0, rh * 0.5, 0, 0, 0, 0, 1, 0, cx, cy, 0.05, 1]);
    const radii = new Float32Array([12, 12, 12, 12]);
    Primitives.fillRectRounded(renderer, m, radii, 0, rgbaPm(30, 41, 59, 235));

    // Title strip in the drag region.
    Primitives.fillRect(renderer, 0, 0, W, 32, 0.04, rgbaPm(15, 23, 42, 245));
    Primitives.drawText(
      renderer,
      12,
      9,
      0.03,
      'transparent + frameless',
      13,
      rgba(226, 232, 240, 255),
    );

    renderer.endFrame();

    ++frame;
    if (frame >= 30) {
      self.close();
    }
  },
  { fps: 60 },
);
