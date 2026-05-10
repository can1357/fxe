import type { Theme } from './theme.ts';

const spacing = Object.freeze({ xs: 4, sm: 8, md: 12, lg: 16, xl: 24 });
const radii = Object.freeze({ sm: 4, md: 8, lg: 14, pill: 999 });
const fontSizes = Object.freeze({ sm: 12, md: 16, lg: 20, xl: 28 });
const lineHeights = Object.freeze({ sm: 16, md: 20, lg: 24, xl: 32 });
const monoFamily = 'SFMono-Regular, Consolas, "Liberation Mono", Menlo, monospace';
const houseFontFamily = 'Inter, "Segoe UI", sans-serif';
const macosFontFamily = '-apple-system, "SF Pro Text", "Helvetica Neue", sans-serif';
const win11FontFamily = '"Segoe UI Variable", "Segoe UI", sans-serif';

function freezeTheme(theme: Theme): Theme {
  return Object.freeze(theme);
}

export const lightTheme: Theme = freezeTheme({
  colors: {
    background: 0xf6f8fcff,
    surface: 0xffffffff,
    surfaceElevated: 0xfafcffd9,
    surfaceTranslucent: 0xf6f8fccc,
    primary: 0x3068ffff,
    primaryText: 0xffffffff,
    text: 0x1f2937ff,
    mutedText: 0x667085ff,
    border: 0xd7deebff,
    accent: 0x3068ffff,
    accentMuted: 0xdbe6ffff,
    focusRing: 0x7ca2ffff,
    disabled: 0xe7ecf5ff,
    disabledText: 0x94a0b5ff,
    success: 0x198754ff,
    warning: 0xd97706ff,
    danger: 0xdc3545ff,
  },
  spacing,
  radii,
  fontSizes,
  typography: {
    fontFamily: houseFontFamily,
    monoFamily,
    lineHeights,
  },
  elevation: {
    none: { offsetY: 0, blur: 0, spread: 0, color: 0x00000000 },
    sm: { offsetY: 1, blur: 4, spread: 0, color: 0x13203c14 },
    md: { offsetY: 6, blur: 18, spread: -4, color: 0x13203c1f },
    lg: { offsetY: 12, blur: 32, spread: -8, color: 0x13203c29 },
  },
  motion: {
    durationFast: 120,
    durationStandard: 180,
    durationSlow: 280,
    easingStandard: [0.2, 0, 0, 1],
  },
  chrome: {
    titleBarHeight: 30,
    trafficLightGutter: 0,
    useSystemAccent: false,
    backdrop: 'none',
  },
});

export const darkTheme: Theme = freezeTheme({
  colors: {
    background: 0x0c1018ff,
    surface: 0x171d29ff,
    surfaceElevated: 0x202838ff,
    surfaceTranslucent: 0x171d29d9,
    primary: 0x5b8cffff,
    primaryText: 0xffffffff,
    text: 0xf4f6fbff,
    mutedText: 0xa7afc2ff,
    border: 0x30384dff,
    accent: 0x5b8cffff,
    accentMuted: 0x233458ff,
    focusRing: 0x86aaffff,
    disabled: 0x222a39ff,
    disabledText: 0x778097ff,
    success: 0x3ddc97ff,
    warning: 0xffb454ff,
    danger: 0xff5b6eff,
  },
  spacing,
  radii,
  fontSizes,
  typography: {
    fontFamily: houseFontFamily,
    monoFamily,
    lineHeights,
  },
  elevation: {
    none: { offsetY: 0, blur: 0, spread: 0, color: 0x00000000 },
    sm: { offsetY: 2, blur: 8, spread: 0, color: 0x00000026 },
    md: { offsetY: 10, blur: 24, spread: -6, color: 0x00000040 },
    lg: { offsetY: 18, blur: 40, spread: -10, color: 0x00000052 },
  },
  motion: {
    durationFast: 120,
    durationStandard: 180,
    durationSlow: 280,
    easingStandard: [0.2, 0, 0, 1],
  },
  chrome: {
    titleBarHeight: 30,
    trafficLightGutter: 0,
    useSystemAccent: false,
    backdrop: 'none',
  },
});

export const macosLightTheme: Theme = freezeTheme({
  ...lightTheme,
  colors: {
    ...lightTheme.colors,
    surfaceElevated: 0xffffffff,
    surfaceTranslucent: 0xf7f9fde0,
    border: 0xd3dbe8f0,
  },
  typography: {
    ...lightTheme.typography,
    fontFamily: macosFontFamily,
  },
  chrome: {
    titleBarHeight: 28,
    trafficLightGutter: 80,
    useSystemAccent: false,
    backdrop: 'vibrancy',
  },
});

export const macosDarkTheme: Theme = freezeTheme({
  ...darkTheme,
  colors: {
    ...darkTheme.colors,
    surfaceElevated: 0x222a38ff,
    surfaceTranslucent: 0x151925d9,
    border: 0x3a445bd9,
  },
  typography: {
    ...darkTheme.typography,
    fontFamily: macosFontFamily,
  },
  chrome: {
    titleBarHeight: 28,
    trafficLightGutter: 80,
    useSystemAccent: false,
    backdrop: 'vibrancy',
  },
});

export const win11LightTheme: Theme = freezeTheme({
  ...lightTheme,
  typography: {
    ...lightTheme.typography,
    fontFamily: win11FontFamily,
  },
  chrome: {
    titleBarHeight: 32,
    trafficLightGutter: 0,
    useSystemAccent: true,
    backdrop: 'mica',
  },
});

export const win11DarkTheme: Theme = freezeTheme({
  ...darkTheme,
  typography: {
    ...darkTheme.typography,
    fontFamily: win11FontFamily,
  },
  chrome: {
    titleBarHeight: 32,
    trafficLightGutter: 0,
    useSystemAccent: true,
    backdrop: 'mica',
  },
});
