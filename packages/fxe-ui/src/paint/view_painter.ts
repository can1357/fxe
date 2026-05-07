import { type CommandBuffer, type Mat4, Primitives } from 'fxe';
import type { LayoutResult } from '../layout/types.ts';
import type { PaintStyle } from '../style/types.ts';

type PaintViewContext = { screenWidth: number; screenHeight: number };

type BorderSide = 'top' | 'right' | 'bottom' | 'left';
type BorderStyle = NonNullable<PaintStyle['borderStyle']>;

const DEFAULT_BORDER_COLOR = 0xffffffff;
const DASH_LENGTH = 6;
const DASH_GAP = 4;
const DOT_LENGTH = 1;
const DOT_GAP = 2;

export function paintView(
  cb: CommandBuffer,
  rect: LayoutResult,
  paint: PaintStyle,
  ctx: PaintViewContext,
): void {
  const x = rect.x;
  const y = rect.y;
  const w = rect.width;
  const h = rect.height;
  if (w <= 0 || h <= 0) return;

  paintShadow(cb, x, y, w, h, paint, ctx);

  const rounded = hasRoundedCorners(paint);
  if (paint.backgroundColor !== undefined) {
    if (rounded) {
      Primitives.fillRectRounded(
        cb,
        rectMatrix(x, y, w, h),
        borderRadii(paint),
        0,
        paint.backgroundColor,
      );
    } else {
      Primitives.fillRect(cb, x, y, w, h, 0, paint.backgroundColor);
    }
  }

  if (paint.borderStyle === 'none') return;

  const borderStyle = paint.borderStyle ?? 'solid';
  const segmented = borderStyle === 'dashed' || borderStyle === 'dotted';
  if (hasPerSideBorder(paint) || segmented) {
    paintPerSideBorders(cb, x, y, w, h, paint, borderStyle, rounded);
    return;
  }

  const borderWidth = paint.borderWidth ?? 0;
  if (borderWidth > 0) {
    const borderColor = paint.borderColor ?? DEFAULT_BORDER_COLOR;
    if (rounded) {
      Primitives.drawRectRounded(
        cb,
        rectMatrix(x, y, w, h),
        borderRadii(paint),
        0,
        borderColor,
        borderWidth,
      );
    } else {
      Primitives.drawRect(cb, x, y, w, h, 0, borderColor, borderWidth);
    }
  }
}

function paintShadow(
  cb: CommandBuffer,
  x: number,
  y: number,
  w: number,
  h: number,
  paint: PaintStyle,
  ctx: PaintViewContext,
): void {
  if (paint.shadowColor === undefined) return;
  const screenWidth = Math.max(0, ctx.screenWidth);
  const screenHeight = Math.max(0, ctx.screenHeight);
  if (screenWidth === 0 || screenHeight === 0) return;
  const blur = paint.shadowBlur ?? 0;
  const spread = paint.shadowSpread ?? 0;
  const offsetX = paint.shadowOffsetX ?? 0;
  const offsetY = paint.shadowOffsetY ?? 0;
  if (hasRoundedCorners(paint)) {
    Primitives.drawShadowRectRounded(
      cb,
      x,
      y,
      w,
      h,
      0,
      borderRadii(paint),
      paint.shadowColor,
      blur,
      spread,
      offsetX,
      offsetY,
      screenWidth,
      screenHeight,
    );
  } else {
    Primitives.drawShadowRect(
      cb,
      x,
      y,
      w,
      h,
      0,
      paint.shadowColor,
      blur,
      spread,
      offsetX,
      offsetY,
      screenWidth,
      screenHeight,
    );
  }
}

function paintPerSideBorders(
  cb: CommandBuffer,
  x: number,
  y: number,
  w: number,
  h: number,
  paint: PaintStyle,
  style: BorderStyle,
  rounded: boolean,
): void {
  paintBorderSide(cb, 'top', x, y, w, h, paint, style, rounded);
  paintBorderSide(cb, 'right', x, y, w, h, paint, style, rounded);
  paintBorderSide(cb, 'bottom', x, y, w, h, paint, style, rounded);
  paintBorderSide(cb, 'left', x, y, w, h, paint, style, rounded);
}

function paintBorderSide(
  cb: CommandBuffer,
  side: BorderSide,
  x: number,
  y: number,
  w: number,
  h: number,
  paint: PaintStyle,
  style: BorderStyle,
  rounded: boolean,
): void {
  const width = sideWidth(paint, side);
  if (width <= 0) return;
  const color = sideColor(paint, side);
  // Dashed/dotted rounded borders require arc decomposition that lands in P3a.
  const effectiveStyle = rounded && style !== 'solid' ? 'solid' : style;
  if (effectiveStyle === 'solid') {
    fillSolidBorderSide(cb, side, x, y, w, h, width, color);
  } else {
    fillSegmentedBorderSide(cb, side, x, y, w, h, width, color, effectiveStyle);
  }
}

function fillSolidBorderSide(
  cb: CommandBuffer,
  side: BorderSide,
  x: number,
  y: number,
  w: number,
  h: number,
  width: number,
  color: NonNullable<PaintStyle['borderColor']>,
): void {
  const half = width * 0.5;
  switch (side) {
    case 'top':
      Primitives.fillRect(cb, x - half, y - half, Math.max(0, w + width), width, 0, color);
      return;
    case 'right':
      Primitives.fillRect(cb, x + w - half, y - half, width, Math.max(0, h + width), 0, color);
      return;
    case 'bottom':
      Primitives.fillRect(cb, x - half, y + h - half, Math.max(0, w + width), width, 0, color);
      return;
    case 'left':
      Primitives.fillRect(cb, x - half, y - half, width, Math.max(0, h + width), 0, color);
      return;
  }
}

function fillSegmentedBorderSide(
  cb: CommandBuffer,
  side: BorderSide,
  x: number,
  y: number,
  w: number,
  h: number,
  width: number,
  color: NonNullable<PaintStyle['borderColor']>,
  style: Exclude<BorderStyle, 'solid' | 'none'>,
): void {
  const segment = style === 'dashed' ? DASH_LENGTH : DOT_LENGTH;
  const gap = style === 'dashed' ? DASH_GAP : DOT_GAP;
  const length = side === 'top' || side === 'bottom' ? w : h;
  if (length <= 0) return;
  for (let offset = 0; offset < length; offset += segment + gap) {
    fillBorderSegment(
      cb,
      side,
      x,
      y,
      w,
      h,
      width,
      color,
      offset,
      Math.min(segment, length - offset),
    );
  }
}

function fillBorderSegment(
  cb: CommandBuffer,
  side: BorderSide,
  x: number,
  y: number,
  w: number,
  h: number,
  width: number,
  color: NonNullable<PaintStyle['borderColor']>,
  offset: number,
  length: number,
): void {
  const half = width * 0.5;
  const run = Math.max(0, length);
  switch (side) {
    case 'top':
      Primitives.fillRect(cb, x + offset, y - half, run, width, 0, color);
      return;
    case 'right':
      Primitives.fillRect(cb, x + w - half, y + offset, width, run, 0, color);
      return;
    case 'bottom':
      Primitives.fillRect(cb, x + offset, y + h - half, run, width, 0, color);
      return;
    case 'left':
      Primitives.fillRect(cb, x - half, y + offset, width, run, 0, color);
      return;
  }
}

function hasPerSideBorder(paint: PaintStyle): boolean {
  return (
    paint.borderTopWidth !== undefined ||
    paint.borderRightWidth !== undefined ||
    paint.borderBottomWidth !== undefined ||
    paint.borderLeftWidth !== undefined ||
    paint.borderTopColor !== undefined ||
    paint.borderRightColor !== undefined ||
    paint.borderBottomColor !== undefined ||
    paint.borderLeftColor !== undefined
  );
}

function sideWidth(paint: PaintStyle, side: BorderSide): number {
  switch (side) {
    case 'top':
      return paint.borderTopWidth ?? paint.borderWidth ?? 0;
    case 'right':
      return paint.borderRightWidth ?? paint.borderWidth ?? 0;
    case 'bottom':
      return paint.borderBottomWidth ?? paint.borderWidth ?? 0;
    case 'left':
      return paint.borderLeftWidth ?? paint.borderWidth ?? 0;
  }
}

function sideColor(paint: PaintStyle, side: BorderSide): NonNullable<PaintStyle['borderColor']> {
  switch (side) {
    case 'top':
      return paint.borderTopColor ?? paint.borderColor ?? DEFAULT_BORDER_COLOR;
    case 'right':
      return paint.borderRightColor ?? paint.borderColor ?? DEFAULT_BORDER_COLOR;
    case 'bottom':
      return paint.borderBottomColor ?? paint.borderColor ?? DEFAULT_BORDER_COLOR;
    case 'left':
      return paint.borderLeftColor ?? paint.borderColor ?? DEFAULT_BORDER_COLOR;
  }
}

function hasRoundedCorners(paint: PaintStyle): boolean {
  return (
    (paint.borderRadius ?? 0) > 0 ||
    (paint.borderTopLeftRadius ?? 0) > 0 ||
    (paint.borderTopRightRadius ?? 0) > 0 ||
    (paint.borderBottomRightRadius ?? 0) > 0 ||
    (paint.borderBottomLeftRadius ?? 0) > 0
  );
}

function borderRadii(paint: PaintStyle): Float32Array {
  const fallback = Math.max(0, paint.borderRadius ?? 0);
  return new Float32Array([
    Math.max(0, paint.borderTopLeftRadius ?? fallback),
    Math.max(0, paint.borderTopRightRadius ?? fallback),
    Math.max(0, paint.borderBottomRightRadius ?? fallback),
    Math.max(0, paint.borderBottomLeftRadius ?? fallback),
  ]);
}

function rectMatrix(x: number, y: number, w: number, h: number): Mat4 {
  return new Float32Array([w, 0, 0, 0, 0, h, 0, 0, 0, 0, 1, 0, x, y, 0, 0]);
}
