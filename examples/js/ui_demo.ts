import { Primitives, Renderer, Window } from 'fxe';

const rgba = (r: number, g: number, b: number, a = 255): number =>
  ((r & 255) << 24) | ((g & 255) << 16) | ((b & 255) << 8) | (a & 255);

class UiCanvas {
  constructor(private readonly renderer: Renderer) {}

  panel(x: number, y: number, w: number, h: number, title: string): void {
    Primitives.fillRect(this.renderer, x, y, w, h, 0.1, rgba(17, 24, 39, 235));
    Primitives.fillRect(this.renderer, x, y, w, 36, 0.09, rgba(31, 41, 55, 245));
    Primitives.drawText(this.renderer, x + 16, y + 10, 0.08, title, 16, rgba(226, 232, 240));
  }

  button(x: number, y: number, w: number, h: number, label: string, active = false): void {
    const fill = active ? rgba(37, 99, 235, 255) : rgba(55, 65, 81, 255);
    const text = active ? rgba(255, 255, 255, 255) : rgba(203, 213, 225, 255);
    Primitives.fillRect(this.renderer, x, y, w, h, 0.05, fill);
    Primitives.drawText(this.renderer, x + 14, y + 10, 0.04, label, 14, text);
  }

  progress(x: number, y: number, w: number, label: string, value: number, color: number): void {
    const clamped = Math.max(0, Math.min(1, value));
    Primitives.drawText(this.renderer, x, y, 0.04, label, 13, rgba(203, 213, 225));
    Primitives.fillRect(this.renderer, x, y + 22, w, 10, 0.04, rgba(30, 41, 59, 255));
    Primitives.fillRect(this.renderer, x, y + 22, w * clamped, 10, 0.03, color);
  }

  card(x: number, y: number, w: number, h: number, headline: string, body: string): void {
    Primitives.fillRect(this.renderer, x, y, w, h, 0.06, rgba(15, 23, 42, 220));
    Primitives.drawText(this.renderer, x + 12, y + 12, 0.05, headline, 15, rgba(248, 250, 252));
    Primitives.drawText(this.renderer, x + 12, y + 38, 0.05, body, 12, rgba(148, 163, 184));
  }
}

const win = new Window({ width: 900, height: 540, title: 'fxe ui demo' });
const renderer = new Renderer(win);
const ui = new UiCanvas(renderer);

win.run((self) => {
  const [w, h] = self.framebufferSize();

  renderer.beginFrame();
  Primitives.fillRect(renderer, 0, 0, w, h, 0.2, rgba(2, 6, 23));
  ui.panel(40, 36, 820, 468, 'FXE Control Surface');
  ui.card(64, 96, 240, 110, 'Frame', 'GPU backend: Dawn / WebGPU');
  ui.card(330, 96, 240, 110, 'Scripting', 'Runtime: embedded V8');
  ui.card(596, 96, 240, 110, 'Assets', 'Spritesheet + stb font atlas');
  ui.progress(64, 246, 330, 'Command buffer fill', 0.72, rgba(34, 197, 94));
  ui.progress(64, 300, 330, 'Bloom threshold', 0.38, rgba(250, 204, 21));
  ui.progress(64, 354, 330, 'TS runtime strip budget', 0.86, rgba(96, 165, 250));
  ui.button(596, 256, 110, 42, 'Start', true);
  ui.button(720, 256, 110, 42, 'Capture');
  ui.button(596, 316, 110, 42, 'Reload');
  ui.button(720, 316, 110, 42, 'Settings');
  Primitives.drawText(
    renderer,
    596,
    412,
    0.04,
    'Status: one-frame retained-mode mock UI',
    14,
    rgba(125, 211, 252),
  );
  renderer.endFrame();
});
