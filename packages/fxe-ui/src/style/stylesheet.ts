import type { Style } from './types.ts';

export const STYLE_SHEET_BRAND: unique symbol = Symbol('fxe-ui.stylesheet');

export type FrozenStyle<T extends Style> = Readonly<T> & { readonly [STYLE_SHEET_BRAND]?: true };

export function create<T extends Record<string, Style>>(
  styles: T,
): { readonly [K in keyof T]: FrozenStyle<T[K]> } {
  const out: Partial<Record<keyof T, FrozenStyle<Style>>> = {};
  for (const key of Object.keys(styles) as Array<keyof T>) {
    out[key] = freezeStyle({ ...styles[key] }) as FrozenStyle<Style>;
  }
  return Object.freeze(out) as { readonly [K in keyof T]: FrozenStyle<T[K]> };
}

function freezeStyle<T extends Style>(style: T): FrozenStyle<T> {
  Object.defineProperty(style, STYLE_SHEET_BRAND, {
    value: true,
    enumerable: false,
    configurable: false,
    writable: false,
  });
  return Object.freeze(style) as FrozenStyle<T>;
}

export const StyleSheet = { create } as const;
