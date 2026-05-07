import type { CursorKind, Color as FxeColor, Paint as FxePaint } from 'fxe';
import type { LayoutStyle, Length } from '../layout/types.ts';

export type Color = number | readonly [number, number, number, number] | `#${string}`;
export type Paint = FxeColor | FxePaint;
export type { Length };

export interface PaintStyle {
  backgroundColor?: FxePaint;
  opacity?: number;
  tint?: FxeColor;
  borderWidth?: number;
  borderColor?: FxeColor;
  borderTopWidth?: number;
  borderRightWidth?: number;
  borderBottomWidth?: number;
  borderLeftWidth?: number;
  borderTopColor?: FxeColor;
  borderRightColor?: FxeColor;
  borderBottomColor?: FxeColor;
  borderLeftColor?: FxeColor;
  borderStyle?: 'solid' | 'dashed' | 'dotted' | 'none';
  shadowColor?: FxeColor;
  shadowOffsetX?: number;
  shadowOffsetY?: number;
  shadowBlur?: number;
  shadowSpread?: number;
  borderRadius?: number;
  borderTopLeftRadius?: number;
  borderTopRightRadius?: number;
  borderBottomLeftRadius?: number;
  borderBottomRightRadius?: number;
  cursor?: CursorKind;
  pointerEvents?: 'auto' | 'none';
}

export interface TextStyle {
  color?: FxeColor;
  fontSize?: number;
  fontFamily?: string;
  fontWeight?: number;
  lineHeight?: number;
  textAlign?: 'left' | 'center' | 'right';
  letterSpacing?: number;
}

export interface Style extends LayoutStyle {
  backgroundColor?: Color | FxePaint;
  opacity?: number;
  tint?: Color;
  borderWidth?: number;
  borderColor?: Color;
  borderTopWidth?: number;
  borderRightWidth?: number;
  borderBottomWidth?: number;
  borderLeftWidth?: number;
  borderTopColor?: Color;
  borderRightColor?: Color;
  borderBottomColor?: Color;
  borderLeftColor?: Color;
  borderStyle?: 'solid' | 'dashed' | 'dotted' | 'none';
  shadowColor?: Color;
  shadowOffsetX?: number;
  shadowOffsetY?: number;
  shadowBlur?: number;
  shadowSpread?: number;
  borderRadius?: number;
  borderTopLeftRadius?: number;
  borderTopRightRadius?: number;
  borderBottomLeftRadius?: number;
  borderBottomRightRadius?: number;
  color?: Color;
  fontSize?: number;
  fontFamily?: string;
  fontWeight?: number;
  lineHeight?: number;
  textAlign?: 'left' | 'center' | 'right';
  letterSpacing?: number;
  cursor?: CursorKind;
  pointerEvents?: 'auto' | 'none';
}

export type StyleValue = Style | readonly StyleValue[] | null | undefined | false;
export type ResolvedStyle = Required<Pick<Style, never>> & Style;
