import type { LayoutStyle } from '../layout/types.ts';
import { parseColor } from './color.ts';
import type { PaintStyle, Style, StyleValue, TextStyle } from './types.ts';

const layoutKeys = new Set<keyof LayoutStyle>([
  'display',
  'width',
  'height',
  'minWidth',
  'minHeight',
  'maxWidth',
  'maxHeight',
  'padding',
  'paddingX',
  'paddingY',
  'paddingTop',
  'paddingRight',
  'paddingBottom',
  'paddingLeft',
  'margin',
  'marginX',
  'marginY',
  'marginTop',
  'marginRight',
  'marginBottom',
  'marginLeft',
  'flexDirection',
  'flexWrap',
  'justifyContent',
  'alignItems',
  'alignSelf',
  'alignContent',
  'flex',
  'flexGrow',
  'flexShrink',
  'flexBasis',
  'gap',
  'rowGap',
  'columnGap',
  'position',
  'top',
  'right',
  'bottom',
  'left',
  'aspectRatio',
  'overflow',
]);
const textKeys = new Set<keyof TextStyle>([
  'color',
  'fontSize',
  'fontFamily',
  'fontWeight',
  'lineHeight',
  'textAlign',
  'letterSpacing',
]);
const paintKeys = new Set<keyof PaintStyle>([
  'backgroundColor',
  'opacity',
  'tint',
  'borderWidth',
  'borderColor',
  'borderTopWidth',
  'borderRightWidth',
  'borderBottomWidth',
  'borderLeftWidth',
  'borderTopColor',
  'borderRightColor',
  'borderBottomColor',
  'borderLeftColor',
  'borderStyle',
  'shadowColor',
  'shadowOffsetX',
  'shadowOffsetY',
  'shadowBlur',
  'shadowSpread',
  'borderRadius',
  'borderTopLeftRadius',
  'borderTopRightRadius',
  'borderBottomLeftRadius',
  'borderBottomRightRadius',
  'cursor',
  'pointerEvents',
]);

export function flattenStyle(value: StyleValue): Style {
  if (!value) return {};
  if (Array.isArray(value)) return Object.assign({}, ...value.map(flattenStyle));
  return value;
}

// Memo cache for splitStyle. StyleSheet.create() returns frozen, stable
// object refs that flow through every component render; splitting them
// into layout/paint/text buckets is deterministic per ref. WeakMap so
// transient style objects can still be GC'd.
//
// Hot in scenes with many identical components (stress grid: 900 cells
// × splitStyle on render = 900 splits per frame, all identical inputs).
// Cache hits skip the Object.entries walk + Set lookups + 3 output objects.
type SplitResult = { layout: LayoutStyle; paint: PaintStyle; text: TextStyle };
const SPLIT_CACHE = new WeakMap<object, SplitResult>();

export function splitStyle(value: StyleValue): {
  layout: LayoutStyle;
  paint: PaintStyle;
  text: TextStyle;
} {
  // Fast path: cached result for stable object refs (the StyleSheet.create()
  // case). Arrays of styles or fresh literals every render skip this.
  if (value !== null && typeof value === 'object' && !Array.isArray(value)) {
    const cached = SPLIT_CACHE.get(value as object);
    if (cached !== undefined) return cached;
  }
  const flat = flattenStyle(value);
  const layout: LayoutStyle = {};
  const paint: PaintStyle = {};
  const text: TextStyle = {};
  for (const [rawKey, rawValue] of Object.entries(flat) as Array<[keyof Style, unknown]>) {
    if (rawValue === undefined) continue;
    if (layoutKeys.has(rawKey as keyof LayoutStyle)) {
      (layout as Record<string, unknown>)[rawKey] = rawValue;
    } else if (textKeys.has(rawKey as keyof TextStyle)) {
      (text as Record<string, unknown>)[rawKey] =
        rawKey === 'color' ? parseColor(rawValue as never) : rawValue;
    } else if (paintKeys.has(rawKey as keyof PaintStyle)) {
      (paint as Record<string, unknown>)[rawKey] =
        rawKey === 'backgroundColor' && isGradientPaint(rawValue)
          ? rawValue
          : paintColorKeys.has(rawKey as keyof PaintStyle)
            ? parseColor(rawValue as never)
            : rawValue;
    } else {
      throw new TypeError(`unsupported fxe-ui style property: ${String(rawKey)}`);
    }
  }
  validateLayout(layout);
  const result: SplitResult = { layout, paint, text };
  if (value !== null && typeof value === 'object' && !Array.isArray(value)) {
    SPLIT_CACHE.set(value as object, result);
  }
  return result;
}

const paintColorKeys = new Set<keyof PaintStyle>([
  'backgroundColor',
  'borderColor',
  'borderTopColor',
  'borderRightColor',
  'borderBottomColor',
  'borderLeftColor',
  'tint',
  'shadowColor',
]);

function isGradientPaint(value: unknown): boolean {
  return typeof value === 'object' && value !== null && '__fxePaint' in value && 'stops' in value;
}

function validateLayout(layout: LayoutStyle): void {
  if (layout.display !== undefined && layout.display !== 'flex' && layout.display !== 'none') {
    throw new TypeError(`fxe-ui supports display: 'flex' or 'none', got ${String(layout.display)}`);
  }
}
