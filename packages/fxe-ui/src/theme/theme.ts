import type { Color } from '../style/types.ts';
import { darkTheme } from './presets.ts';

export type ElevationToken = {
  offsetY: number;
  blur: number;
  spread: number;
  color: Color;
};

export interface Theme {
  colors: {
    background: Color;
    surface: Color;
    surfaceElevated: Color;
    surfaceTranslucent: Color;
    primary: Color;
    primaryText: Color;
    text: Color;
    mutedText: Color;
    border: Color;
    accent: Color;
    accentMuted: Color;
    focusRing: Color;
    disabled: Color;
    disabledText: Color;
    success: Color;
    warning: Color;
    danger: Color;
  };
  spacing: { xs: number; sm: number; md: number; lg: number; xl: number };
  radii: { sm: number; md: number; lg: number; pill: number };
  fontSizes: { sm: number; md: number; lg: number; xl: number };
  typography: {
    fontFamily: string;
    monoFamily: string;
    lineHeights: { sm: number; md: number; lg: number; xl: number };
  };
  elevation: {
    none: ElevationToken;
    sm: ElevationToken;
    md: ElevationToken;
    lg: ElevationToken;
  };
  motion: {
    durationFast: number;
    durationStandard: number;
    durationSlow: number;
    easingStandard: [number, number, number, number];
  };
  chrome: {
    titleBarHeight: number;
    trafficLightGutter: number;
    useSystemAccent: boolean;
    backdrop: 'none' | 'mica' | 'acrylic' | 'vibrancy';
  };
}

export * from './presets.ts';

export const defaultTheme: Theme = darkTheme;
