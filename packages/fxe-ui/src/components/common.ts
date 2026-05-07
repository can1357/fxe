import type { CommandBuffer } from 'fxe';
import { layout } from '../layout/index.ts';
import type { LayoutNode, LayoutResult } from '../layout/types.ts';
import { type BoundaryChild, Draw, Layer, type Node } from '../reconciler/fiber.ts';
import { flattenStyle, splitStyle } from '../style/resolve.ts';
import type { Style, StyleValue, TextStyle } from '../style/types.ts';

export interface InternalLayoutProps {
  __layout?: LayoutResult;
  __textStyle?: TextStyle;
}

export interface UIBaseProps extends InternalLayoutProps {
  key?: string;
  style?: StyleValue;
  children?: BoundaryChild;
}

export function normalizeChildren(child: BoundaryChild): Node[] {
  if (child === null || child === undefined || typeof child === 'boolean') return [];
  if (Array.isArray(child)) return child.flatMap((entry) => normalizeChildren(entry));
  return [child];
}

export function childStyle(node: Node): Style {
  if (node.type === 'component' && typeof node.props === 'object' && node.props !== null) {
    return flattenStyle((node.props as { style?: StyleValue }).style);
  }
  return {};
}

export function childText(node: Node): string | undefined {
  if (node.type === 'component' && typeof node.props === 'object' && node.props !== null) {
    const children = (node.props as { children?: unknown }).children;
    if (typeof children === 'string' || typeof children === 'number') return String(children);
  }
  return undefined;
}

export function cloneWithInternal(
  node: Node,
  layoutResult: LayoutResult,
  textStyle?: TextStyle,
): Node {
  if (node.type !== 'component') return node;
  return {
    ...node,
    props: {
      ...(node.props as Record<string, unknown>),
      __layout: layoutResult,
      __textStyle: textStyle,
    },
  };
}

export function layoutChildren(
  children: readonly Node[],
  width: number,
  height: number,
): LayoutResult[] {
  const root: LayoutNode = {
    style: { width, height },
    children: children.map((child) => {
      const text = childText(child);
      const style = splitStyle(childStyle(child)).layout;
      return {
        style,
        measure:
          text === undefined
            ? undefined
            : () => ({
                width:
                  text.length *
                  (Number((childStyle(child) as { fontSize?: number }).fontSize ?? 16) * 0.55),
                height: Number((childStyle(child) as { fontSize?: number }).fontSize ?? 16) * 1.2,
              }),
      };
    }),
  };
  return layout(root, { width, height }).children;
}

export function layerWithDraw(
  fn: (cb: CommandBuffer) => void,
  children: readonly Node[],
  deps: ReadonlyArray<unknown>,
): Node {
  return Layer({ deps, children: [Draw(fn, deps), ...children] });
}

export function rectFromStyle(style: Style, fallback: LayoutResult | undefined): LayoutResult {
  const width = typeof style.width === 'number' ? style.width : (fallback?.width ?? 0);
  const height = typeof style.height === 'number' ? style.height : (fallback?.height ?? 0);
  return {
    x: typeof style.left === 'number' ? style.left : (fallback?.x ?? 0),
    y: typeof style.top === 'number' ? style.top : (fallback?.y ?? 0),
    width,
    height,
    paddingLeft:
      typeof style.paddingLeft === 'number'
        ? style.paddingLeft
        : typeof style.padding === 'number'
          ? style.padding
          : 0,
    paddingTop:
      typeof style.paddingTop === 'number'
        ? style.paddingTop
        : typeof style.padding === 'number'
          ? style.padding
          : 0,
    paddingRight:
      typeof style.paddingRight === 'number'
        ? style.paddingRight
        : typeof style.padding === 'number'
          ? style.padding
          : 0,
    paddingBottom:
      typeof style.paddingBottom === 'number'
        ? style.paddingBottom
        : typeof style.padding === 'number'
          ? style.padding
          : 0,
    children: [],
  };
}
