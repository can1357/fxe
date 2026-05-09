import { CommandBuffer } from 'fxe';
import { extractA11yProps } from '../a11y/extract.ts';
import type { AccessibilityProps } from '../a11y/types.ts';
import { recordLayout } from '../debug/layout_trace.ts';
import { layout } from '../layout/index.ts';
import type { LayoutNode, LayoutResult, LayoutStyle } from '../layout/types.ts';
import { type HitTarget, hitTargets, registerHitTarget } from '../mount/hit_test.ts';
import { currentRenderTargetSize } from '../mount/mount.ts';
import { coarseClip } from '../paint/clip.ts';
import { paintView } from '../paint/view_painter.ts';
import {
  type BoundaryChild,
  Component,
  type ContextFrameSnapshot,
  currentContextFrames,
  type Node,
  Portal,
  useId,
  withContextFrames,
} from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue, TextStyle } from '../style/types.ts';
import { wrapText } from '../text/wrap.ts';
import { TextStyleContext } from '../theme/text_context.ts';
import {
  cloneWithInternal,
  type InternalLayoutProps,
  normalizeChildren,
  rectFromStyle,
} from './common.ts';

export interface ViewProps extends InternalLayoutProps, AccessibilityProps {
  key?: string;
  style?: StyleValue;
  children?: BoundaryChild;
}
type ViewInternalProps = ViewProps & { __skipA11yHitTarget?: boolean };

type ComponentProps = {
  style?: StyleValue;
  children?: unknown;
  __textStyle?: TextStyle;
};
function isDisplayNone(node: Node): boolean {
  if (node.type !== 'component') return false;
  return resolveStyleMeta((node.props as ComponentProps).style).resolved.layout.display === 'none';
}

function textFromChildren(child: unknown): string {
  if (child === null || child === undefined || typeof child === 'boolean') return '';
  if (typeof child === 'string' || typeof child === 'number') return String(child);
  if (Array.isArray(child)) return child.map(textFromChildren).join('');
  return '';
}

function normalizeLayoutChildren(child: unknown): Node[] {
  if (
    child === null ||
    child === undefined ||
    typeof child === 'boolean' ||
    typeof child === 'string' ||
    typeof child === 'number'
  ) {
    return [];
  }
  if (Array.isArray(child)) return child.flatMap(normalizeLayoutChildren);
  return typeof child === 'object' && 'type' in child ? [child as Node] : [];
}

// Ask the wrapping helper for a real measurement: when the parent constrains
// our cross-axis width, we wrap inside that constraint and report the
// resulting multi-line box.
function measureText(
  text: string,
  style: TextStyle,
): (constraint: { width?: number; height?: number }) => { width: number; height: number } {
  return (constraint) => {
    const wrapped = wrapText(text, style, { maxWidth: constraint.width });
    return { width: wrapped.width, height: wrapped.height };
  };
}

function childrenOf(node: Node): readonly Node[] {
  if (node.type === 'layer') return node.props.children;
  if (node.type === 'provider') return normalizeChildren(node.props.children);
  if (node.type === 'error-boundary' || node.type === 'suspense') {
    return normalizeChildren(node.props.children);
  }
  return [];
}

function clipChildHitTargets(start: number, clip: LayoutResult): void {
  const targets = hitTargets() as HitTarget[];
  for (let i = targets.length - 1; i >= start; --i) {
    const target = targets[i];
    const left = Math.max(target.rect.x, clip.x);
    const top = Math.max(target.rect.y, clip.y);
    const right = Math.min(target.rect.x + target.rect.width, clip.x + clip.width);
    const bottom = Math.min(target.rect.y + target.rect.height, clip.y + clip.height);
    if (right <= left || bottom <= top) {
      targets.splice(i, 1);
    } else {
      target.rect = {
        ...target.rect,
        x: left,
        y: top,
        width: right - left,
        height: bottom - top,
      };
    }
  }
}

// Stable per-style metadata keyed by the original style object identity. This
// lets View/layoutNodeFor short-circuit splitStyle + layoutStyleSig together
// when callers reuse the same style ref across renders.
const kStyleMeta = Symbol('fxe-ui.styleMeta');
const kLayoutStyleSig = Symbol('fxe-ui.layoutStyleSig');

// Content-derived signature for a LayoutStyle. We cache by ref first so
// stable StyleSheet.create() refs hit immediately; on a fresh ref (e.g.
// the synthetic object produced by `[s.cell, {bg}]` going through
// splitStyle) we fall back to a JSON of the layout-affecting fields.
// Two different objects with identical layout content share a sig, so
// the layout cache can reuse cells whose only delta is paint state.
//
// Field list mirrors the LayoutStyle keys in style/resolve.ts. Adding
// a layout-affecting field there requires adding it here too.
const LAYOUT_SIG_FIELDS: readonly (keyof LayoutStyle)[] = [
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
];

function layoutStyleSig(style: LayoutStyle | undefined): string {
  if (style === undefined) return '0';
  const cached = Reflect.get(style, kLayoutStyleSig);
  if (cached !== undefined) return cached as string;
  let sig = '';
  for (let i = 0; i < LAYOUT_SIG_FIELDS.length; ++i) {
    const k = LAYOUT_SIG_FIELDS[i];
    const v = style[k];
    if (v === undefined) continue;
    sig += `${i}:${typeof v === 'object' ? JSON.stringify(v) : String(v)};`;
  }
  // Memoize so repeat refs don't re-walk. New ref with identical content
  // pays the walk once but still produces an interned string V8 will
  // canonicalize.
  const id = sig === '' ? '0' : sig;
  Reflect.set(style, kLayoutStyleSig, id);
  return id;
}

function resolveStyleMeta(style: StyleValue): {
  resolved: ReturnType<typeof splitStyle>;
  layoutSig: string;
} {
  if (style !== null && typeof style === 'object') {
    const cached = Reflect.get(style, kStyleMeta) as
      | { resolved: ReturnType<typeof splitStyle>; layoutSig: string }
      | undefined;
    if (cached !== undefined) return cached;
    const resolved = splitStyle(style);
    const meta = { resolved, layoutSig: layoutStyleSig(resolved.layout) };
    Reflect.set(style, kStyleMeta, meta);
    return meta;
  }
  const resolved = splitStyle(style);
  return { resolved, layoutSig: layoutStyleSig(resolved.layout) };
}

function textStyleSig(style: TextStyle): string {
  // Only the layout-affecting fields. Color etc. don't change layout.
  return `${style.fontSize ?? 16}|${style.letterSpacing ?? 0}|${style.lineHeight ?? -1}|${style.fontFamily ?? ''}|${style.fontWeight ?? ''}`;
}

const DIRECT_LAYOUT_COMPONENTS = new Set([
  'EditableArea',
  'Gutter',
  'Image',
  'LineViewport',
  'Pressable',
  'ScrollView',
  'Text',
  'TextArea',
  'TextInput',
  'View',
  'VirtualList',
]);

function isDirectLayoutComponent(displayName: string | undefined): boolean {
  return displayName !== undefined && DIRECT_LAYOUT_COMPONENTS.has(displayName);
}

function layoutNodeFor(
  node: Node,
  inheritedTextStyle: TextStyle,
  contextFrames: readonly ContextFrameSnapshot[] = currentContextFrames(),
): LayoutNode {
  if (node.type === 'component') {
    if (!isDirectLayoutComponent(node.displayName)) {
      const produced = withContextFrames(contextFrames, () => node.render(node.props));
      return layoutNodeFor(produced, inheritedTextStyle, contextFrames);
    }
    const childProps = node.props as ComponentProps;
    const { resolved, layoutSig } = resolveStyleMeta(childProps.style);
    const textStyle = {
      ...inheritedTextStyle,
      ...(childProps.__textStyle ?? {}),
      ...resolved.text,
    };
    if (node.displayName === 'Text') {
      const text = textFromChildren(childProps.children);
      return {
        style: resolved.layout,
        measure: measureText(text, textStyle),
        // Sig: leaf depends on layout style + text content + text-style
        // fields that affect layout. resolveStyleMeta() memoizes both
        // splitStyle() and layoutStyleSig() by the original style ref.
        _sig: `T|${layoutSig}|${textStyleSig(textStyle)}|${text}`,
      };
    }
    const childLayoutNodes = normalizeLayoutChildren(childProps.children).map((child) =>
      layoutNodeFor(child, textStyle, contextFrames),
    );
    let allChildSigs = '';
    let anyMissing = false;
    for (let i = 0; i < childLayoutNodes.length; ++i) {
      const cs = childLayoutNodes[i]._sig;
      if (cs === undefined) {
        anyMissing = true;
        break;
      }
      allChildSigs += i === 0 ? cs : `~${cs}`;
    }
    return {
      style: resolved.layout,
      children: childLayoutNodes,
      // Only emit a sig when every child contributed one; otherwise it
      // wouldn't be safe to memoize against.
      _sig: anyMissing ? undefined : `V|${layoutSig}|${allChildSigs}`,
    };
  }

  if (node.type === 'provider') {
    const nextFrames = [...contextFrames, { ctx: node.props.ctx, value: node.props.value }];
    const children = normalizeChildren(node.props.children);
    return children.length
      ? { children: children.map((child) => layoutNodeFor(child, inheritedTextStyle, nextFrames)) }
      : {};
  }

  const children = childrenOf(node);
  return children.length
    ? { children: children.map((child) => layoutNodeFor(child, inheritedTextStyle, contextFrames)) }
    : {};
}

export const View = Component((props: ViewProps): Node => {
  const internalProps = props as ViewInternalProps;
  const id = useId();
  const { resolved } = resolveStyleMeta(props.style);
  const rect = props.__layout
    ? { ...props.__layout }
    : rectFromStyle(resolved.layout, props.__layout);
  if (!props.__layout && (rect.width === 0 || rect.height === 0)) {
    rect.width = typeof resolved.layout.width === 'number' ? resolved.layout.width : 0;
    rect.height = typeof resolved.layout.height === 'number' ? resolved.layout.height : 0;
  }
  recordLayout({
    component: 'View',
    rect: { ...rect },
    hasParentLayout: !!props.__layout,
    styleWidth: resolved.layout.width,
    styleHeight: resolved.layout.height,
    tag: (props as { __traceTag?: string }).__traceTag,
  });
  if (!internalProps.__skipA11yHitTarget) {
    registerHitTarget({
      id,
      rect,
      a11y: extractA11yProps(props),
      componentType: 'View',
      tabIndex: props.tabIndex,
      focusGroup: undefined,
    });
  }
  const rawChildren = normalizeChildren(props.children);
  const textStyle = { ...(props.__textStyle ?? {}), ...resolved.text };
  const visibleChildren = rawChildren.filter((child) => !isDisplayNone(child));
  const childTree =
    props.__layout && props.__layout.children.length === visibleChildren.length
      ? props.__layout
      : layout(
          {
            style: { ...resolved.layout, width: rect.width, height: rect.height },
            children: visibleChildren.map((child) => layoutNodeFor(child, textStyle)),
          },
          { width: rect.width, height: rect.height },
        );
  const children = visibleChildren.map((child, idx) => {
    const childRect = childTree.children[idx] ?? {
      x: 0,
      y: 0,
      width: 0,
      height: 0,
      paddingLeft: 0,
      paddingTop: 0,
      paddingRight: 0,
      paddingBottom: 0,
      children: [],
    };
    return cloneWithInternal(
      child,
      { ...childRect, x: rect.x + childRect.x, y: rect.y + childRect.y },
      textStyle,
    );
  });
  const targetSize = currentRenderTargetSize();
  const paintNode = {
    type: 'draw' as const,
    props: {
      fn: (cb: CommandBuffer) =>
        paintView(cb, rect, resolved.paint, {
          screenWidth: targetSize.width,
          screenHeight: targetSize.height,
        }),
      deps: [
        props.style,
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        targetSize.width,
        targetSize.height,
      ],
    },
  } satisfies Node;
  const childNode = TextStyleContext.Provider({ value: textStyle, children });
  const layerChildren: Node[] = [paintNode];
  if (resolved.layout.overflow === 'hidden' || resolved.layout.overflow === 'scroll') {
    const childBuffer = new CommandBuffer();
    let hitTargetStart = 0;
    const markHitTargetStart = {
      type: 'draw' as const,
      props: {
        fn: () => {
          hitTargetStart = hitTargets().length;
        },
        deps: [
          props.style,
          rect.x,
          rect.y,
          rect.width,
          rect.height,
          targetSize.width,
          targetSize.height,
        ],
      },
    } satisfies Node;
    const clippedNode = {
      type: 'draw' as const,
      props: {
        fn: (cb: CommandBuffer) => {
          clipChildHitTargets(hitTargetStart, rect);
          const clipped = coarseClip(childBuffer, rect);
          if (!clipped.isEmpty()) cb.queue(clipped);
        },
        deps: [
          props.style,
          rect.x,
          rect.y,
          rect.width,
          rect.height,
          targetSize.width,
          targetSize.height,
        ],
      },
    } satisfies Node;
    layerChildren.push(
      markHitTargetStart,
      Portal({ to: childBuffer, children: childNode }),
      clippedNode,
    );
  } else {
    layerChildren.push(childNode);
  }

  const layer = {
    type: 'layer' as const,
    props: {
      deps: [
        props.style,
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        targetSize.width,
        targetSize.height,
      ],
      children: layerChildren,
    },
  } satisfies Node;
  return layer;
}, 'View');
