import { Primitives } from 'fxe';
import type { Constraint } from './types.ts';

export function measureText(
  text: string,
  fontSize = 16,
): (constraint: Constraint) => { width: number; height: number } {
  return () => {
    const [width, height] = Primitives.calcText(text, fontSize);
    return { width, height };
  };
}

export function measureImage(
  width: number,
  height: number,
): (constraint: Constraint) => { width: number; height: number } {
  return () => ({ width, height });
}
