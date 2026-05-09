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
  type Fiber,
  findVisibleChildFiber,
  getCurrentRenderFiber,
  type Node,
  Portal,
  requestRenderTargetRedraw,
  shallowEqualProps,
  useId,
  useInternalLayout,
  useInternalTextStyle,
} from '../reconciler/fiber.ts';
import { splitStyle } from '../style/resolve.ts';
import type { StyleValue, TextStyle } from '../style/types.ts';
import { wrapText } from '../text/wrap.ts';
import { TextStyleContext } from '../theme/text_context.ts';
import { attachInternalLayout, normalizeChildren, rectFromStyle } from './common.ts';

export interface ViewProps extends AccessibilityProps {
  key?: string;
  style?: StyleValue;
  children?: BoundaryChild;
  __traceTag?: string;
}
type ViewInternalProps = ViewProps & { __skipA11yHitTarget?: boolean };

type ComponentProps = {
  style?: StyleValue;
  children?: unknown;
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
const kTextStyleSig = Symbol('fxe-ui.textStyleSig');

// Content-derived signature for a LayoutStyle. We cache by ref first so
// stable StyleSheet.create() refs hit immediately; on a fresh ref (e.g.
// the synthetic object produced by `[s.cell, {bg}]` going through
// splitStyle) we fall back to a JSON of the layout-affecting fields.
// Two different objects with identical layout content share a sig, so
// the layout cache can reuse cells whose only delta is paint state.
//
// Field list mirrors the LayoutStyle keys in style/resolve.ts. Adding
// a layout-affecting field there requires adding it here too.
const LAYOUT_SIG_FIELDS = new Set([
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
]);

function layoutStyleSig(style: LayoutStyle | undefined): string {
  if (style === undefined) return '0';
  const cached = Reflect.get(style, kLayoutStyleSig);
  if (cached !== undefined) return cached as string;
  let sig = '';
  for (const k in style) {
    const v = style[k as keyof LayoutStyle];
    if (!v) continue;
    if (LAYOUT_SIG_FIELDS.has(k as keyof LayoutStyle)) {
      sig += `${k}:${typeof v === 'object' ? JSON.stringify(v) : String(v)};`;
    }
  }
  const id = sig === '' ? '0' : sig;
  Reflect.set(style, kLayoutStyleSig, id);
  return id;
}

function resolveStyleMeta(style: StyleValue): {
  resolved: ReturnType<typeof splitStyle>;
  layoutSig: string;
  paintSig: string;
} {
  if (style !== null && typeof style === 'object') {
    const cached = Reflect.get(style, kStyleMeta) as
      | { resolved: ReturnType<typeof splitStyle>; layoutSig: string; paintSig: string }
      | undefined;
    if (cached !== undefined) return cached;
    const resolved = splitStyle(style);
    const layoutSig = layoutStyleSig(resolved.layout);
    // Paint sig captures the fields paintView reads. Stringify to a
    // stable primitive so the Layer deps array compares by value, not
    // object reference. Cached on the (potentially short-lived) style
    // object via Reflect.set; canonical StyleSheet objects keep this
    // hot across all renders, inline `{...}` objects pay the hash once.
    const p = resolved.paint as Record<string, unknown>;
    const paintSig = `${p.backgroundColor ?? ''}|${p.borderColor ?? ''}|${p.borderWidth ?? ''}|${p.borderRadius ?? ''}|${p.borderTopLeftRadius ?? ''}|${p.borderTopRightRadius ?? ''}|${p.borderBottomLeftRadius ?? ''}|${p.borderBottomRightRadius ?? ''}|${p.opacity ?? ''}|${p.tint ?? ''}|${p.shadowColor ?? ''}|${p.shadowOffset ?? ''}|${p.shadowRadius ?? ''}|${p.shadowOpacity ?? ''}`;
    const meta = { resolved, layoutSig, paintSig };
    Reflect.set(style, kStyleMeta, meta);
    return meta;
  }
  const resolved = splitStyle(style);
  return {
    resolved,
    layoutSig: layoutStyleSig(resolved.layout),
    paintSig: '',
  };
}

function textStyleSig(style: TextStyle): string {
  // Only the layout-affecting fields. Color etc. don't change layout.
  if (!style || typeof style !== 'object') return '';
  const cached = Reflect.get(style, kTextStyleSig);
  if (cached !== undefined) return cached as string;
  const sig = `${style.fontSize ?? 16}|${style.letterSpacing ?? 0}|${style.lineHeight ?? -1}|${style.fontFamily ?? ''}|${style.fontWeight ?? ''}`;
  Reflect.set(style, kTextStyleSig, sig);
  return sig;
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

// Layout-side memo. The render-time memo bail saves paint, but layoutNodeFor
// still walks every non-direct component to derive layout. The stable cache
// now lives on the child fiber; the symbol on `node.props` remains a same-pass
// fallback when the same props object is revisited within one layout walk.
interface LayoutMemoEntry {
  values: unknown[];
  produced: Node;
}
const kLayoutMemo = Symbol('fxe-ui.layoutMemo');

function firstChildFiber(parent: Fiber): Fiber | null {
  if (parent.childOrder.length === 0) return null;
  return parent.children.get(parent.childOrder[0]) ?? null;
}

function snapshotContextValues(frames: readonly ContextFrameSnapshot[]): unknown[] {
  const out: unknown[] = new Array(frames.length);
  for (let i = 0; i < frames.length; ++i) out[i] = frames[i].value;
  return out;
}

function contextValuesMatch(cached: unknown[], frames: readonly ContextFrameSnapshot[]): boolean {
  if (cached.length !== frames.length) return false;
  for (let i = 0; i < cached.length; ++i) {
    if (cached[i] !== frames[i].value) return false;
  }
  return true;
}

function layoutNodeListSig(nodes: readonly LayoutNode[]): { sig: string; complete: boolean } {
  let sig = '';
  let complete = true;
  for (let i = 0; i < nodes.length; ++i) {
    const childSig = nodes[i]._sig;
    if (childSig === undefined) complete = false;
    sig += i === 0 ? (childSig ?? '?') : `~${childSig ?? '?'}`;
  }
  return { sig, complete };
}

function layoutResultSig(result: LayoutResult): string {
  let sig = `${result.x},${result.y},${result.width},${result.height},${result.paddingLeft},${result.paddingTop},${result.paddingRight},${result.paddingBottom}`;
  for (let i = 0; i < result.children.length; ++i) {
    sig += `|${layoutResultSig(result.children[i])}`;
  }
  return sig;
}

const kLayoutComplete = Symbol('fxe-ui.layoutComplete');

function isLayoutComplete(result: LayoutResult | null | undefined): boolean {
  return result === null || result === undefined || Reflect.get(result, kLayoutComplete) !== false;
}

function markLayoutCompleteness(result: LayoutResult, node: LayoutNode): boolean {
  const childNodes = node.children ?? [];
  let complete = node._sig !== undefined;
  for (let i = 0; i < result.children.length; ++i) {
    const childNode = childNodes[i];
    if (childNode !== undefined && !markLayoutCompleteness(result.children[i], childNode)) {
      complete = false;
    }
  }
  Reflect.set(result, kLayoutComplete, complete);
  return complete;
}

function absoluteLayoutResult(result: LayoutResult, x: number, y: number): LayoutResult {
  const out = { ...result, x, y };
  Reflect.set(out, kLayoutComplete, isLayoutComplete(result));
  return out;
}

function layoutNodeFor(
  node: Node,
  inheritedTextStyle: TextStyle,
  contextFrames: readonly ContextFrameSnapshot[] = currentContextFrames(),
  parentFiber: Fiber | null = null,
): LayoutNode {
  if (node.type === 'component') {
    if (!isDirectLayoutComponent(node.displayName)) {
      // Stabilise props identity ahead of the reconciler's own swap.
      // The reconciler's `pendingProps` swap fires when it descends to
      // this fiber later in the frame; at this point the layout walk is
      // RUNNING ahead of the reconciler, so without a local swap the
      // identity-keyed caches below would always miss against
      // last-frame's `lastProps` even when the props are shallow-equal.
      if (
        parentFiber !== null &&
        parentFiber.lastProps !== undefined &&
        parentFiber.lastProps !== node.props
      ) {
        const eq =
          node.memo !== undefined
            ? node.memo.areEqual(parentFiber.lastProps, node.props)
            : shallowEqualProps(parentFiber.lastProps, node.props);
        if (eq) (node as { props: unknown }).props = parentFiber.lastProps;
      }
      const cachedLayout = parentFiber?.layoutCache;
      if (
        cachedLayout !== undefined &&
        cachedLayout.props === node.props &&
        contextValuesMatch(cachedLayout.values, contextFrames)
      ) {
        return cachedLayout.layoutNode as LayoutNode;
      }
      let produced: Node | null = null;
      // Primary cache: previous render stored produced JSX on the child
      // fiber. After the swap above, `parentFiber.lastProps === node.props`
      // when shallow-equal so we use it directly. We intentionally allow
      // a one-frame stale produced subtree while the current render is still
      // rebuilding: that preserves the previous layout instead of collapsing
      // composite children to 0x0 for a frame, and the next frame sees the
      // updated produced tree.
      if (
        parentFiber !== null &&
        parentFiber.lastProducedNode !== null &&
        parentFiber.lastProps === node.props
      ) {
        produced = parentFiber.lastProducedNode;
      }
      // Within-pass dedupe (same props ref hit twice in one pass).
      if (produced === null) {
        const memoCacheKey =
          typeof node.props === 'object' && node.props !== null ? (node.props as object) : null;
        if (memoCacheKey !== null) {
          const entry = Reflect.get(memoCacheKey, kLayoutMemo) as LayoutMemoEntry | undefined;
          if (entry !== undefined && contextValuesMatch(entry.values, contextFrames)) {
            produced = entry.produced;
          }
        }
      }
      // No cached produced node. We deliberately do NOT run the body here:
      // doing so during a layout walk either (a) corrupts the parent fiber's
      // hook state, or (b) requires a sandbox fiber whose useMemo cache is
      // empty — which forces every memoised computation (e.g. tree-sitter
      // highlight) to recompute on every layout walk. Both are catastrophic.
      //
      // Instead, return an unresolved LayoutNode until this component has
      // rendered once. If the component already rendered to null for the
      // current props, treat that as a resolved empty subtree so static
      // null components do not force an endless redraw loop.
      if (produced === null) {
        if (
          parentFiber !== null &&
          parentFiber.lastProps === node.props &&
          parentFiber.lastProducedNode === null
        ) {
          return { _sig: `C0|${node.displayName ?? ''}` };
        }
        return {};
      }
      // Walk into produced through the existing fiber subtree so the
      // recursion keeps hitting cache. parentFiber's reconciled child
      // (created by previous render) corresponds to `produced`.
      const innerFiber =
        parentFiber !== null
          ? (findVisibleChildFiber(parentFiber, produced, 0) ?? firstChildFiber(parentFiber))
          : null;
      const result = layoutNodeFor(produced, inheritedTextStyle, contextFrames, innerFiber);
      if (parentFiber) {
        parentFiber.layoutCache = {
          props: node.props,
          values: snapshotContextValues(contextFrames),
          layoutNode: result,
        };
      }
      return result;
    }
    const childProps = node.props as ComponentProps;
    const { resolved, layoutSig } = resolveStyleMeta(childProps.style);
    const textStyle = {
      ...inheritedTextStyle,
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
    const childLayoutNodes = normalizeLayoutChildren(childProps.children).map((child, index) =>
      layoutNodeFor(
        child,
        textStyle,
        contextFrames,
        parentFiber ? findVisibleChildFiber(parentFiber, child, index) : null,
      ),
    );
    const childSig = layoutNodeListSig(childLayoutNodes);
    return {
      style: resolved.layout,
      children: childLayoutNodes,
      // Only emit a sig when every child contributed one; otherwise it
      // wouldn't be safe to memoize against.
      _sig: childSig.complete ? `V|${layoutSig}|${childSig.sig}` : undefined,
    };
  }

  if (node.type === 'provider') {
    const nextFrames: readonly ContextFrameSnapshot[] = [
      ...contextFrames,
      { ctx: node.props.ctx as ContextFrameSnapshot['ctx'], value: node.props.value },
    ];
    const children = normalizeChildren(node.props.children);
    const childLayoutNodes = children.map((child, index) =>
      layoutNodeFor(
        child,
        inheritedTextStyle,
        nextFrames,
        parentFiber ? findVisibleChildFiber(parentFiber, child, index) : null,
      ),
    );
    const childSig = layoutNodeListSig(childLayoutNodes);
    return children.length
      ? {
          children: childLayoutNodes,
          _sig: childSig.complete ? `P|${childSig.sig}` : undefined,
        }
      : { _sig: 'P|' };
  }

  const children = childrenOf(node);
  const childLayoutNodes = children.map((child, index) =>
    layoutNodeFor(
      child,
      inheritedTextStyle,
      contextFrames,
      parentFiber ? findVisibleChildFiber(parentFiber, child, index) : null,
    ),
  );
  const childSig = layoutNodeListSig(childLayoutNodes);
  return children.length
    ? {
        children: childLayoutNodes,
        _sig: childSig.complete ? `${node.type}|${childSig.sig}` : undefined,
      }
    : { _sig: `${node.type}|` };
}

export const View = Component((props: ViewProps): Node => {
  const internalProps = props as ViewInternalProps;
  const id = useId();
  const { resolved, layoutSig, paintSig } = resolveStyleMeta(props.style);
  const inheritedLayout = useInternalLayout();
  const inheritedTextStyle = useInternalTextStyle();
  const rect = inheritedLayout ? { ...inheritedLayout } : rectFromStyle(resolved.layout, undefined);
  if (!inheritedLayout && (rect.width === 0 || rect.height === 0)) {
    rect.width = typeof resolved.layout.width === 'number' ? resolved.layout.width : 0;
    rect.height = typeof resolved.layout.height === 'number' ? resolved.layout.height : 0;
  }
  recordLayout({
    component: 'View',
    rect: { ...rect },
    hasParentLayout: !!inheritedLayout,
    styleWidth: resolved.layout.width,
    styleHeight: resolved.layout.height,
    tag: props.__traceTag,
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
  const textStyle = { ...(inheritedTextStyle ?? {}), ...resolved.text };
  const visibleChildren = rawChildren.filter((child) => !isDisplayNone(child));
  const viewFiber = getCurrentRenderFiber();
  let childLayoutNodes: LayoutNode[] | null = null;
  let childLayoutSig = '';
  let childLayoutComplete = true;
  const buildChildLayoutNodes = (): LayoutNode[] => {
    if (childLayoutNodes !== null) return childLayoutNodes;
    childLayoutNodes = visibleChildren.map((child, index) =>
      layoutNodeFor(
        child,
        textStyle,
        currentContextFrames(),
        viewFiber ? findVisibleChildFiber(viewFiber, child, index) : null,
      ),
    );
    const sig = layoutNodeListSig(childLayoutNodes);
    childLayoutSig = sig.sig;
    childLayoutComplete = sig.complete;
    return childLayoutNodes;
  };
  const useInheritedLayout =
    inheritedLayout &&
    inheritedLayout.children.length === visibleChildren.length &&
    isLayoutComplete(inheritedLayout);
  const childTree = useInheritedLayout
    ? inheritedLayout
    : (() => {
        const rootLayoutNode = {
          style: { ...resolved.layout, width: rect.width, height: rect.height },
          children: buildChildLayoutNodes(),
          _sig: childLayoutComplete ? `R|${childLayoutSig}` : undefined,
        };
        const computed = layout(rootLayoutNode, { width: rect.width, height: rect.height });
        childLayoutComplete = markLayoutCompleteness(computed, rootLayoutNode);
        return computed;
      })();
  if (!useInheritedLayout && !childLayoutComplete) requestRenderTargetRedraw();
  const resolvedLayoutSig = useInheritedLayout
    ? `I|${layoutResultSig(childTree)}`
    : `L|${childLayoutSig}|${layoutResultSig(childTree)}`;
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
    return attachInternalLayout(
      child,
      absoluteLayoutResult(childRect, rect.x + childRect.x, rect.y + childRect.y),
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
        layoutSig,
        paintSig,
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
          layoutSig,
          paintSig,
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
          layoutSig,
          paintSig,
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

  const layerDeps = childLayoutComplete
    ? [
        layoutSig,
        paintSig,
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        targetSize.width,
        targetSize.height,
        resolvedLayoutSig,
      ]
    : undefined;

  const layer = {
    type: 'layer' as const,
    props: {
      deps: layerDeps,
      children: layerChildren,
    },
  } satisfies Node;
  return layer;
}, 'View');
