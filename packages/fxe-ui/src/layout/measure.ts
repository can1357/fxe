import type { Constraint } from './types.ts';

// Tagged measure function. The layout bridge reads these tags to forward
// the measurement intent to the C++ Layout solver without round-tripping
// through JS each time Yoga needs a size.
export interface TaggedMeasureFn {
  (constraint: Constraint): { width: number; height: number };
  __fxeMeasureKind?: 'text' | 'image';
  __fxeMeasureText?: string;
  __fxeMeasureFontSize?: number;
  __fxeMeasureWidth?: number;
  __fxeMeasureHeight?: number;
}

export function measureText(text: string, fontSize = 16): TaggedMeasureFn {
  // The function body still works as a JS fallback (the bridge prefers the
  // tag and avoids calling it).
  const fn: TaggedMeasureFn = () => {
    const [width, height] = Primitives.calcText(text, fontSize);
    return { width, height };
  };
  fn.__fxeMeasureKind = 'text';
  fn.__fxeMeasureText = text;
  fn.__fxeMeasureFontSize = fontSize;
  return fn;
}

export function measureImage(width: number, height: number): TaggedMeasureFn {
  const fn: TaggedMeasureFn = () => ({ width, height });
  fn.__fxeMeasureKind = 'image';
  fn.__fxeMeasureWidth = width;
  fn.__fxeMeasureHeight = height;
  return fn;
}
