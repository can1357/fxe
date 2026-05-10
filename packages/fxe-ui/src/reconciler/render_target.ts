import type { Window } from 'fxe';

let g_render_target: Window | null = null;

export function setRenderTarget(win: Window | null): void {
  g_render_target = win;
}

export function getRenderTarget(): Window | null {
  return g_render_target;
}
