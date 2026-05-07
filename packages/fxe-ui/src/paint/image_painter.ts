import { type CommandBuffer, Primitives } from 'fxe';
import type { LayoutResult } from '../layout/types.ts';
import type { PaintStyle } from '../style/types.ts';

export function paintImage(cb: CommandBuffer, rect: LayoutResult, paint: PaintStyle): void {
  if (rect.width <= 0 || rect.height <= 0) return;
  Primitives.fillRect(cb, rect.x, rect.y, rect.width, rect.height, 0, paint.tint ?? 0xffffffff);
}
