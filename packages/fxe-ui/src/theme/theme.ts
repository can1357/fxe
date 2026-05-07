import type { Color } from '../style/types.ts';

export interface Theme {
  colors: {
    background: Color;
    surface: Color;
    primary: Color;
    primaryText: Color;
    text: Color;
    mutedText: Color;
    border: Color;
    danger: Color;
  };
  spacing: { xs: number; sm: number; md: number; lg: number; xl: number };
  radii: { sm: number; md: number; lg: number; pill: number };
  fontSizes: { sm: number; md: number; lg: number; xl: number };
}

export const defaultTheme: Theme = Object.freeze({
  colors: {
    background: 0x10131aff,
    surface: 0x1b2030ff,
    primary: 0x5b8cffff,
    primaryText: 0xffffffff,
    text: 0xf4f6fbff,
    mutedText: 0xa7afc2ff,
    border: 0x30384dff,
    danger: 0xff5b6eff,
  },
  spacing: { xs: 4, sm: 8, md: 12, lg: 16, xl: 24 },
  radii: { sm: 4, md: 8, lg: 14, pill: 999 },
  fontSizes: { sm: 12, md: 16, lg: 20, xl: 28 },
});
