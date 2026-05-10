import type { Color, Theme } from 'fxe-ui';
import {
  darkTheme,
  defaultTheme,
  lightTheme,
  macosDarkTheme,
  macosLightTheme,
  win11DarkTheme,
  win11LightTheme,
} from 'fxe-ui';
import { assert, assertEqual, run, test } from './ts_harness.ts';

const requiredColorKeys = [
  'background',
  'surface',
  'surfaceElevated',
  'surfaceTranslucent',
  'primary',
  'primaryText',
  'text',
  'mutedText',
  'border',
  'accent',
  'accentMuted',
  'focusRing',
  'disabled',
  'disabledText',
  'success',
  'warning',
  'danger',
] as const satisfies readonly (keyof Theme['colors'])[];

const presets = {
  lightTheme,
  darkTheme,
  macosLightTheme,
  macosDarkTheme,
  win11LightTheme,
  win11DarkTheme,
} as const satisfies Record<string, Theme>;

function toRgba8(color: Color): readonly [number, number, number, number] {
  if (typeof color === 'number') {
    return [(color >>> 24) & 0xff, (color >>> 16) & 0xff, (color >>> 8) & 0xff, color & 0xff];
  }
  if (typeof color === 'string') {
    const hex = color.slice(1);
    if (hex.length === 6 || hex.length === 8) {
      const value = Number.parseInt(hex, 16);
      if (hex.length === 6) {
        return [(value >>> 16) & 0xff, (value >>> 8) & 0xff, value & 0xff, 0xff];
      }
      return [(value >>> 24) & 0xff, (value >>> 16) & 0xff, (value >>> 8) & 0xff, value & 0xff];
    }
    if (hex.length === 3 || hex.length === 4) {
      const [r, g, b, a = 'f'] = hex;
      return [
        Number.parseInt(r + r, 16),
        Number.parseInt(g + g, 16),
        Number.parseInt(b + b, 16),
        Number.parseInt(a + a, 16),
      ];
    }
    throw new Error(`unsupported color literal: ${color}`);
  }
  const [r, g, b, a] = color;
  return [r, g, b, a];
}

function compositeOver(fg: Color, bg: Color): readonly [number, number, number] {
  const [fr, fgChannel, fb, fa] = toRgba8(fg);
  const [br, bgChannel, bb] = toRgba8(bg);
  const alpha = fa / 255;
  return [
    Math.round(fr * alpha + br * (1 - alpha)),
    Math.round(fgChannel * alpha + bgChannel * (1 - alpha)),
    Math.round(fb * alpha + bb * (1 - alpha)),
  ];
}

function srgbToLinear(channel: number): number {
  const normalized = channel / 255;
  return normalized <= 0.03928 ? normalized / 12.92 : ((normalized + 0.055) / 1.055) ** 2.4;
}

function relativeLuminance(color: Color, background: Color = 0xffffffff): number {
  const [r, g, b] = compositeOver(color, background);
  return 0.2126 * srgbToLinear(r) + 0.7152 * srgbToLinear(g) + 0.0722 * srgbToLinear(b);
}

function contrastRatio(foreground: Color, background: Color): number {
  const fg = relativeLuminance(foreground, background);
  const bg = relativeLuminance(background);
  const lighter = Math.max(fg, bg);
  const darker = Math.min(fg, bg);
  return (lighter + 0.05) / (darker + 0.05);
}

test('theme presets expose the full color token set', () => {
  for (const [name, theme] of Object.entries(presets)) {
    for (const key of requiredColorKeys) {
      assert(key in theme.colors, `${name} missing colors.${key}`);
      assert(theme.colors[key] !== undefined, `${name} has undefined colors.${key}`);
    }
  }
});

test('light theme keeps body text at AA contrast against the background', () => {
  assert(
    contrastRatio(lightTheme.colors.text, lightTheme.colors.background) >= 4.5,
    'lightTheme.colors.text must maintain WCAG AA contrast on lightTheme.colors.background',
  );
});

test('platform presets carry expected system font families', () => {
  assert(macosLightTheme.typography.fontFamily.includes('-apple-system'));
  assert(macosDarkTheme.typography.fontFamily.includes('-apple-system'));
  assert(win11LightTheme.typography.fontFamily.includes('Segoe UI'));
  assert(win11DarkTheme.typography.fontFamily.includes('Segoe UI'));
});

test('platform chrome presets match their OS-tuned backdrops', () => {
  assertEqual(macosLightTheme.chrome.trafficLightGutter, 80);
  assertEqual(macosDarkTheme.chrome.trafficLightGutter, 80);
  assertEqual(macosLightTheme.chrome.backdrop, 'vibrancy');
  assertEqual(macosDarkTheme.chrome.backdrop, 'vibrancy');
  assertEqual(win11LightTheme.chrome.titleBarHeight, 32);
  assertEqual(win11DarkTheme.chrome.titleBarHeight, 32);
  assertEqual(win11LightTheme.chrome.backdrop, 'mica');
  assertEqual(win11DarkTheme.chrome.backdrop, 'mica');
  assertEqual(win11LightTheme.chrome.useSystemAccent, true);
  assertEqual(win11DarkTheme.chrome.useSystemAccent, true);
});

test('elevation tokens stay non-negative where rendering expects it', () => {
  for (const [name, theme] of Object.entries(presets)) {
    for (const [level, token] of Object.entries(theme.elevation)) {
      assert(token.blur >= 0, `${name}.${level}.blur must be non-negative`);
      assert(token.offsetY >= 0, `${name}.${level}.offsetY must be non-negative`);
    }
  }
});

test('defaultTheme stays aliased to darkTheme for back-compat', () => {
  assertEqual(defaultTheme, darkTheme);
});

run();
