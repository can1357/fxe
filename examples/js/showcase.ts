// A lightly more involved scene that exercises rect + text primitives. The
// window stays open until you close it; redraws happen on demand.

import { Primitives, Renderer, Window } from 'fxe';

const win = new Window({ width: 1280, height: 720, title: 'fxe — showcase' });
const renderer = new Renderer(win);

win.run((self) => {
  const [w, h] = self.framebufferSize();

  renderer.beginFrame();
  Primitives.fillRect(renderer, 0, 0, w, h, 0.2, 0x0b1220ff);
  Primitives.fillRect(renderer, 40, 40, 200, 120, 0.1, 0xffffffff);
  Primitives.drawText(renderer, 60, 80, 0.0, 'fxe', 24, 0x111827ff);
  Primitives.drawText(
    renderer,
    60,
    h - 60,
    0.0,
    'Resize the window — only one extra paint per resize.',
    14,
    0x9ca3afff,
  );
  renderer.endFrame();
});
