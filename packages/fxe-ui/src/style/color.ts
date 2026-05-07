import type { Color as FxeColor } from 'fxe';
import type { Color } from './types.ts';

export function parseColor(value: Color | undefined, fallback?: FxeColor): FxeColor | undefined {
  if (value === undefined) return fallback;
  if (typeof value === 'number') {
    if (!Number.isFinite(value)) throw new TypeError(`color number must be finite: ${value}`);
    return value >>> 0;
  }
  if (Array.isArray(value)) {
    if (value.length !== 4) throw new TypeError('color tuples must be [r,g,b,a]');
    return value.map(channel) as [number, number, number, number];
  }
  if (typeof value === 'string' && value.startsWith('#')) return parseHex(value);
  throw new TypeError(`unsupported color value: ${String(value)}`);
}

function parseHex(value: `#${string}`): number {
  const raw = value.slice(1);
  let hex: string;
  if (raw.length === 3 || raw.length === 4) {
    hex = raw
      .split('')
      .map((ch) => `${ch}${ch}`)
      .join('');
  } else if (raw.length === 6 || raw.length === 8) {
    hex = raw;
  } else {
    throw new TypeError(`hex colors must be #rgb, #rgba, #rrggbb, or #rrggbbaa: ${value}`);
  }
  if (!/^[0-9a-fA-F]+$/.test(hex)) throw new TypeError(`invalid hex color: ${value}`);
  if (hex.length === 6) hex += 'ff';
  return Number.parseInt(hex, 16) >>> 0;
}

function channel(value: number): number {
  if (!Number.isFinite(value) || value < 0 || value > 255) {
    throw new TypeError(`color channels must be finite values in [0,255], got ${value}`);
  }
  return Math.round(value);
}
