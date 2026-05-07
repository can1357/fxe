export type Length = number | `${number}%` | 'auto';

export interface Constraint {
  width?: number;
  height?: number;
}

export type FlexDirection = 'row' | 'column' | 'row-reverse' | 'column-reverse';
export type FlexWrap = 'nowrap' | 'wrap' | 'wrap-reverse';
export type Align = 'auto' | 'flex-start' | 'flex-end' | 'center' | 'stretch' | 'baseline';
export type Justify =
  | 'flex-start'
  | 'flex-end'
  | 'center'
  | 'space-between'
  | 'space-around'
  | 'space-evenly';

export interface LayoutStyle {
  display?: 'flex' | 'none';
  width?: Length;
  height?: Length;
  minWidth?: Length;
  minHeight?: Length;
  maxWidth?: Length;
  maxHeight?: Length;
  padding?: Length;
  paddingX?: Length;
  paddingY?: Length;
  paddingTop?: Length;
  paddingRight?: Length;
  paddingBottom?: Length;
  paddingLeft?: Length;
  margin?: Length;
  marginX?: Length;
  marginY?: Length;
  marginTop?: Length;
  marginRight?: Length;
  marginBottom?: Length;
  marginLeft?: Length;
  flexDirection?: FlexDirection;
  flexWrap?: FlexWrap;
  justifyContent?: Justify;
  alignItems?: Align;
  alignSelf?: Align;
  alignContent?: Align | Justify;
  flex?: number;
  flexGrow?: number;
  flexShrink?: number;
  flexBasis?: Length;
  gap?: number;
  rowGap?: number;
  columnGap?: number;
  position?: 'relative' | 'absolute';
  top?: Length;
  right?: Length;
  bottom?: Length;
  left?: Length;
  aspectRatio?: number;
  overflow?: 'visible' | 'hidden' | 'scroll';
}

export interface LayoutEdges {
  top: number;
  right: number;
  bottom: number;
  left: number;
}

export interface LayoutNode {
  style?: LayoutStyle;
  children?: readonly LayoutNode[];
  measure?: (constraint: Constraint) => { width: number; height: number };
  key?: string;
  // Optional structural signature populated by layoutNodeFor() so the
  // solver can memoize results across frames. Two LayoutNodes with the
  // same _sig laid out under the same constraint produce identical
  // results (modulo top-level x/y, which the caller overwrites). Only
  // populated by trusted producers — solver only consults it, never
  // mutates.
  _sig?: string;
}

export interface LayoutResult {
  x: number;
  y: number;
  width: number;
  height: number;
  paddingLeft: number;
  paddingTop: number;
  paddingRight: number;
  paddingBottom: number;
  children: LayoutResult[];
}
