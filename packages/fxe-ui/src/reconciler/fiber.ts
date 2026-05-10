// Fiber-style reconciler producing fxe CommandBuffers.
//
// Layer({ children, transform?, tint?, deps?, key? })  — cacheable subtree
// Draw(fn, deps?)                                      — leaf draw call
// Component(render, displayName?)                      — function component
// useState / useMemo / useEffect / useFrame / useEvent
// render(node, target, { animate? })                   — drive one frame
// setRenderTarget(win)                                 — wire setState→redraw
// startFrameLoop()                                    — requestAnimationFrame → tickFrame bridge
//
// Each Layer fiber owns a cached CommandBuffer. If its deps array is
// reference-identical-shallow-equal to last frame AND no descendant marked
// dirty since the last render, we queue the cached buffer verbatim and skip
// descent. Otherwise we rebuild into a fresh CommandBuffer.
//
// Hooks store state in per-fiber slots indexed by call order. Reordering or
// changing the slot count between renders throws — same rules-of-hooks as
// React.
// TODO: split into commit.ts / reconcile.ts / cache_invalidation.ts (see TODO.md T1)

import type { Mat4, Vec4, Window, WindowDisposer, WindowEventMap, WindowEventName } from 'fxe';
import { CommandBuffer, Primitives, Renderer } from 'fxe';
import type { LayoutResult } from '../layout/types.ts';
import {
  captureHitTargetsSince,
  type HitTarget,
  hitTargetCount,
  replayHitTargets,
} from '../mount/hit_test.ts';
import type { TextStyle } from '../style/types.ts';
import {
  type DevtoolsFiberNode,
  installFiberTreeSnapshotProvider,
  isPaintFlashEnabled,
  memoTraceState,
} from './devtools.ts';
import type { FrameLoopOptions } from './frame_loop.ts';
import { ensureRenderFrameLoop, g_frame_callbacks, getTickFrameCounter } from './frame_loop.ts';
import {
  getCurrentSchedulerLane,
  installSchedulerHookApi,
  isTransitionFlushActive,
  registerFiberWork,
  scheduleCallback,
  scheduleWork,
  unregisterFiberWork,
} from './scheduler.ts';
import {
  beginFiberSignalTracking,
  endFiberSignalTracking,
  unregisterFiberSignalSubscriptions,
} from './signals.ts';

export type { FrameLoopDisposer, FrameLoopOptions } from './frame_loop.ts';
export { startFrameLoop, tickFrame } from './frame_loop.ts';
export { setRenderTarget } from './render_target.ts';

import { getRenderTarget } from './render_target.ts';
import {
  atlasEpoch,
  bakeFiberSurface,
  releaseSurface,
  SURFACE_BAKE_HITS,
  SURFACE_CACHE_DISABLED,
  SURFACE_MIN_VERTS,
  type SurfaceCacheEntry,
} from './surface_cache.ts';

// `RenderStats` is a host-installed global. The bumpers are no-ops if the
// host hasn't installed install_render_stats_global() yet (we degrade to a
// stub).
declare const RenderStats: {
  snapshot(): {
    verticesSubmitted: number;
    indicesSubmitted: number;
    queueCalls: number;
    cacheHits: number;
    cacheMisses: number;
    rebuilds: number;
    frames: number;
  };
  reset(): void;
  recordCacheHit(): void;
  recordCacheMiss(): void;
  recordRebuild(): void;
  recordQueueCall(): void;
  beginFrame(): void;
};
// ----------------------------------------------------------------- public API

export interface LayerProps {
  key?: string;
  transform?: Mat4;
  tint?: Vec4;
  deps?: ReadonlyArray<unknown>;
  children: readonly Node[];
}

export interface DrawProps {
  fn: (cb: CommandBuffer) => void;
  deps?: ReadonlyArray<unknown>;
}

export type PropsEqual<P> = (prev: Readonly<P>, next: Readonly<P>) => boolean;

interface ComponentMemo {
  areEqual: (prev: unknown, next: unknown) => boolean;
}

type ComponentIdentity = object;

export interface Context<T> {
  readonly defaultValue: T;
  readonly Provider: (props: { key?: string; value: T; children?: BoundaryChild }) => Node;
}

interface AnyContext {
  readonly defaultValue: unknown;
  readonly Provider: (props: { key?: string; value: never; children?: BoundaryChild }) => Node;
}

export interface ContextProviderProps<T> {
  key?: string;
  ctx: Context<T>;
  value: T;
  children?: BoundaryChild;
}

export interface PortalProps {
  key?: string;
  to: CommandBuffer | Renderer;
  children?: BoundaryChild;
}

interface ProviderNodeProps {
  key?: string;
  ctx: AnyContext;
  value: unknown;
  children?: BoundaryChild;
}

export type Node =
  | { type: 'layer'; props: LayerProps; key?: string }
  | { type: 'draw'; props: DrawProps; key?: string }
  | {
      type: 'component';
      componentType?: ComponentIdentity;
      render: (props: unknown) => Node;
      props: unknown;
      displayName?: string;
      key?: string;
      memo?: ComponentMemo;
      // Framework-internal layout state pushed down by parent layout primitives
      // (e.g. View). Read by the child fiber's body via useInternalLayout().
      // Lives outside `props` so user-visible memo bail and shallow-equal
      // comparisons never see it.
      internalLayout?: LayoutResult;
      internalTextStyle?: TextStyle;
    }
  | { type: 'provider'; props: ProviderNodeProps; key?: string }
  | { type: 'portal'; props: PortalProps; key?: string }
  | { type: 'error-boundary'; props: ErrorBoundaryProps; key?: string }
  | { type: 'suspense'; props: SuspenseProps; key?: string };

export function Layer(props: LayerProps): Node {
  return { type: 'layer', props, key: props.key };
}

export function Draw(fn: (cb: CommandBuffer) => void, deps?: ReadonlyArray<unknown>): Node {
  return { type: 'draw', props: { fn, deps } };
}

// ----------------------------------------------------------- component meta
//
// `Component(fn)` and `memo(fn)` return factory functions, but the
// metadata describing how to render an instance lives on the factory
// itself via these symbol-keyed slots. JSX (createElement) and `memo`
// read them so they can build a single component Node directly,
// without an outer-wrapper / inner-component double fiber pair.

export interface FxComponentMeta {
  componentType: ComponentIdentity;
  render: (props: unknown) => Node;
  displayName?: string;
}

export interface FxMemoMeta extends FxComponentMeta {
  areEqual: (prev: unknown, next: unknown) => boolean;
}

export const kFxComponentMeta: unique symbol = Symbol('fxe-ui.componentMeta');
export const kFxMemo: unique symbol = Symbol('fxe-ui.memoMeta');

export function getComponentMeta(value: unknown): FxComponentMeta | undefined {
  if (typeof value !== 'function' && (typeof value !== 'object' || value === null))
    return undefined;
  return (value as { [kFxComponentMeta]?: FxComponentMeta })[kFxComponentMeta];
}

export function getMemoMeta(value: unknown): FxMemoMeta | undefined {
  if (typeof value !== 'function' && (typeof value !== 'object' || value === null))
    return undefined;
  return (value as { [kFxMemo]?: FxMemoMeta })[kFxMemo];
}

export function Component<P>(
  render: (props: P) => Node,
  displayName?: string,
): (props: P & { key?: string }) => Node {
  const componentType: ComponentIdentity = {};
  const innerRender = (raw: unknown): Node => render(raw as P);
  const factory = (props: P & { key?: string }): Node => ({
    type: 'component',
    componentType,
    render: innerRender,
    props,
    displayName,
    key: props.key,
  });
  const meta: FxComponentMeta = { componentType, render: innerRender, displayName };
  Object.defineProperty(factory, kFxComponentMeta, { value: meta, configurable: false });
  // JSX runtime reads the wrapper component's `.name` when computing its
  // displayName (`type.name || 'JSXComponent'`). Arrow functions returned
  // from a higher-order helper get an empty `.name`, so callers like
  // `View` lose track of which intrinsic (Text, View, …) sits inside the
  // JSX wrapper. Stamping the displayName onto the function keeps that
  // identity intact across the JSX layer.
  if (displayName) {
    Object.defineProperty(factory, 'name', { value: displayName, configurable: true });
  }
  return factory;
}

export function shallowEqualProps(prev: unknown, next: unknown): boolean {
  if (Object.is(prev, next)) return true;
  if (typeof prev !== 'object' || prev === null || typeof next !== 'object' || next === null) {
    return false;
  }
  const prevRecord = prev as Record<PropertyKey, unknown>;
  const nextRecord = next as Record<PropertyKey, unknown>;
  const prevKeys = Object.keys(prevRecord);
  const nextKeys = Object.keys(nextRecord);
  if (prevKeys.length !== nextKeys.length) return false;
  for (const key of prevKeys) {
    if (!Object.hasOwn(nextRecord, key)) return false;
    if (!Object.is(prevRecord[key], nextRecord[key])) return false;
  }
  return true;
}

const kLayoutDescSig = Symbol.for('fxe-ui.layoutDescSig');
function layoutResultEqual(prev: LayoutResult | null, next: LayoutResult | null): boolean {
  if (prev === next) return true;
  if (prev === null || next === null) return false;
  if (prev.x !== next.x || prev.y !== next.y) return false;
  const prevDescSig = Reflect.get(prev, kLayoutDescSig);
  const nextDescSig = Reflect.get(next, kLayoutDescSig);
  if (prevDescSig !== undefined && nextDescSig !== undefined) {
    return prevDescSig === nextDescSig;
  }
  if (
    prev.width !== next.width ||
    prev.height !== next.height ||
    prev.paddingLeft !== next.paddingLeft ||
    prev.paddingTop !== next.paddingTop ||
    prev.paddingRight !== next.paddingRight ||
    prev.paddingBottom !== next.paddingBottom ||
    prev.children.length !== next.children.length
  ) {
    return false;
  }
  for (let i = 0; i < prev.children.length; ++i) {
    if (!layoutResultEqual(prev.children[i], next.children[i])) return false;
  }
  return true;
}

export function memo<P>(
  component: (props: P & { key?: string }) => Node,
  areEqual?: PropsEqual<P & { key?: string }>,
): (props: P & { key?: string }) => Node {
  const compare = (areEqual ?? shallowEqualProps) as (prev: unknown, next: unknown) => boolean;
  // If the inner argument is itself a Component(...) factory, unwrap one
  // layer so a memo'd component renders the user body directly. Without
  // this, every `<MemoFoo />` would produce TWO fibers — the memo wrapper
  // plus the inner Foo wrapper — even though both walk the same render
  // function. Matches React's `SimpleMemoComponent`: one fiber per
  // memo'd boundary, identity stabilization at the bail.
  const baseMeta = getComponentMeta(component);
  const componentType: ComponentIdentity = baseMeta?.componentType ?? {};
  const innerRender =
    baseMeta?.render ?? ((raw: unknown): Node => component(raw as P & { key?: string }));
  const displayName = baseMeta?.displayName ?? component.name ?? 'MemoComponent';
  const memoInfo: ComponentMemo = { areEqual: compare };
  const factory = (props: P & { key?: string }): Node => ({
    type: 'component',
    componentType,
    render: innerRender,
    props,
    displayName,
    key: props.key,
    memo: memoInfo,
  });
  const meta: FxMemoMeta = {
    componentType,
    render: innerRender,
    displayName,
    areEqual: compare,
  };
  Object.defineProperty(factory, kFxMemo, { value: meta, configurable: false });
  Object.defineProperty(factory, 'name', { value: displayName, configurable: true });
  return factory;
}

export type BoundaryChild = Node | readonly BoundaryChild[] | null | undefined | boolean;
function isBoundaryChildArray(child: BoundaryChild): child is readonly BoundaryChild[] {
  return Array.isArray(child);
}

export interface ErrorBoundaryProps {
  key?: string;
  children?: BoundaryChild;
  fallback?: BoundaryChild | ((error: unknown) => BoundaryChild);
  onError?: (error: unknown) => void;
}

export interface SuspenseProps {
  key?: string;
  children?: BoundaryChild;
  fallback?: BoundaryChild;
}

function normalizeBoundaryChildren(child: BoundaryChild): Node[] {
  if (child === null || child === undefined || typeof child === 'boolean') return [];
  if (isBoundaryChildArray(child)) {
    return child.flatMap((entry) => normalizeBoundaryChildren(entry));
  }
  return [child];
}

function singleBoundaryNode(child: BoundaryChild): Node {
  const children = normalizeBoundaryChildren(child);
  return children.length === 1 ? children[0] : Layer({ children });
}
// Rebuild a Node variant with a new children array. Generic over T so the
// discriminant survives the spread (TS would otherwise widen `props` to the
// union of every variant and break the type-tag/props pairing).
export function rebuildChildren<T extends Extract<Node, { props: { children?: unknown } }>>(
  node: T,
  children: Node[],
): T {
  return { ...node, props: { ...node.props, children } } as T;
}

function attachInternalToProduced(
  produced: Node,
  layout: LayoutResult | undefined,
  text: TextStyle | undefined,
): Node {
  if (produced.type === 'component') {
    const nextLayout = produced.internalLayout ?? layout;
    const nextText = produced.internalTextStyle ?? text;
    if (nextLayout === produced.internalLayout && nextText === produced.internalTextStyle) {
      return produced;
    }
    return { ...produced, internalLayout: nextLayout, internalTextStyle: nextText };
  }

  if (produced.type === 'layer') {
    if (produced.props.children.length !== 1) return produced;
    const attached = attachInternalToProduced(produced.props.children[0], layout, text);
    return attached === produced.props.children[0]
      ? produced
      : rebuildChildren(produced, [attached]);
  }

  if (
    produced.type === 'provider' ||
    produced.type === 'portal' ||
    produced.type === 'error-boundary' ||
    produced.type === 'suspense'
  ) {
    const children = normalizeBoundaryChildren(produced.props.children);
    if (children.length !== 1) return produced;
    const attached = attachInternalToProduced(children[0], layout, text);
    return attached === children[0] ? produced : rebuildChildren(produced, [attached]);
  }

  return produced;
}

function propagateInternalToProduced(node: Node, produced: Node | null): Node | null {
  if (produced === null || node.type !== 'component') return produced;
  if (node.internalLayout === undefined && node.internalTextStyle === undefined) return produced;
  return attachInternalToProduced(produced, node.internalLayout, node.internalTextStyle);
}

function shouldTraverseMemoProduced(node: Node): boolean {
  return node.type === 'layer' || node.type === 'provider' || node.type === 'portal';
}

export function ErrorBoundary(props: ErrorBoundaryProps): Node {
  return { type: 'error-boundary', props, key: props.key };
}

export function Suspense(props: SuspenseProps): Node {
  return { type: 'suspense', props, key: props.key };
}

export function Portal(props: PortalProps): Node {
  return { type: 'portal', props, key: props.key };
}

export function createContext<T>(defaultValue: T): Context<T> {
  const ctx = {
    defaultValue,
    Provider: (props: { key?: string; value: T; children?: BoundaryChild }): Node => ({
      type: 'provider',
      props: {
        key: props.key,
        ctx: ctx as AnyContext,
        value: props.value,
        children: props.children,
      },
      key: props.key,
    }),
    _consumers: new Set<Fiber>(),
  } satisfies ContextImpl<T>;
  return ctx;
}

// ---------------------------------------------------------------- fiber model

type Cleanup = () => void;

interface EffectSlot {
  kind: 'effect';
  deps: ReadonlyArray<unknown> | undefined;
  cleanup: Cleanup | undefined;
  pending: (() => undefined | Cleanup) | null;
}

interface MemoSlot<T> {
  kind: 'memo';
  deps: ReadonlyArray<unknown>;
  value: T;
}

interface StateSlot<T> {
  kind: 'state';
  value: T;
  setter: (next: T | ((prev: T) => T)) => void;
}

interface ReducerSlot<S, A> {
  kind: 'reducer';
  value: S;
  reducer: (state: S, action: A) => S;
  dispatch: (action: A) => void;
}

interface RefSlot<T> {
  kind: 'ref';
  value: { current: T };
}

interface IdSlot {
  kind: 'id';
  value: string;
}

interface FrameSlot {
  kind: 'frame';
  fn: (dtMs: number) => void;
}

interface EventSlot {
  kind: 'event';
  win: Window;
  evt: WindowEventName;
  dispose: WindowDisposer;
  handlerRef: { current: (ev: unknown) => void };
}

interface ContextImpl<T> extends Context<T> {
  _consumers: Set<Fiber>;
}

interface ContextSlot {
  kind: 'context';
  ctx: ContextImpl<unknown>;
}

interface ContextFrame {
  ctx: ContextImpl<unknown>;
  value: unknown;
}

export interface ContextFrameSnapshot {
  ctx: Context<unknown>;
  value: unknown;
}

// HookSlot is a structural marker only — we tag at runtime with `.kind` and
// the `nextSlot` helper enforces that consecutive renders re-encounter the
// same kind in the same slot. Variance gymnastics around StateSlot<S> /
// MemoSlot<T> would force a single-parameter union; using a base type with
// runtime tags keeps the generic hook signatures sound at every call site.
interface BaseSlot {
  kind: string;
}
type HookSlot = BaseSlot;

export interface Fiber {
  key: string; // structural identity in the parent's child list
  node: Node | null; // last node we reconciled into this fiber
  parent: Fiber | null;
  children: Map<string, Fiber>; // keyed lookup (insertion order = render order)
  childOrder: string[];
  // Subset of `children` whose node.type is 'layer' | 'provider' | 'portal'.
  // findVisibleChildFiber walks these to look through transparent wrappers
  // when the produced subtree skips over them. null until reconcileChildren
  // populates it; an empty array means "no wrappers" — short-circuits the
  // miss branch in the hot path. Maintained by reconcileChildren.
  wrapperChildren: Fiber[] | null;
  // Layer-only:
  cache: CommandBuffer | null;
  lastDeps: ReadonlyArray<unknown> | undefined;
  // Atlas generation observed when `cache` was last built. Glyph-rendering
  // primitives (drawText, drawTextSpans, drawTextRun) bake atlas UVs into
  // vertex data; if the shared glyph cache repacks (LRU eviction under
  // pressure), those UVs go stale and replaying the cached buffer samples
  // arbitrary glyph regions — visible as scrambled text. Stamping the
  // atlas epoch at build time and treating any mismatch as a cache miss
  // forces a clean rebuild against the current atlas layout. Same field
  // serves Layer caches, Component memo caches, and the surface-bake
  // fast path.
  cacheAtlasEpoch: number;
  // Hit targets registered during this Layer's last rebuild. Replayed verbatim
  // when a subsequent frame hits the cached CommandBuffer so click / focus /
  // keyboard targets survive Layer caching (otherwise hit_test would clear and
  // never re-register them on the cached path).
  cachedHitTargets: HitTarget[] | null;
  // Surface caching state. When a Layer's `cache` survives unchanged for
  // SURFACE_BAKE_HITS frames AND the cached buffer's vertex count clears
  // SURFACE_MIN_VERTS, we bake it into an offscreen texture and replace
  // future cache replays with a single drawTextureQuad. surfaceHits is the
  // running stability counter; surface holds the baked offscreen + the
  // user-tex slot it's bound to.
  surfaceHits: number;
  surface: SurfaceCacheEntry | null;
  // Framework-internal layout state pushed down by parent layout primitives
  // before this fiber's body runs. Exposed to the body via useInternalLayout()
  // / useInternalTextStyle(). Lives on the fiber, NOT on user-visible props.
  internalLayout: LayoutResult | null;
  internalTextStyle: TextStyle | null;
  // Component-only:
  hooks: HookSlot[];
  lastProps: unknown;
  lastProducedNode: Node | null;
  layoutCache?: { props: unknown; values: unknown[]; layoutNode: unknown };
  // True when a previous render captured command buffers while layoutNodeFor()
  // still had unresolved memo children. Dirty flags can be cleared while the
  // render stack unwinds, so memo/layer cache bails must consult this durable
  // layout-settled bit as well.
  hasUnresolvedLayout: boolean;
  // Subtree state.
  // Provider-only:
  providedContext: ContextImpl<unknown> | null;
  providedValue: unknown;
  dirty: boolean;
  // Failure flag — a thrown render marks only this subtree.
  failed: boolean;
}

export type FiberCacheHitMiss = 'hit' | 'miss' | null;

export interface FiberNode {
  id: number;
  type: string;
  displayName: string | null;
  key: string;
  props: string;
  propsSummary: string;
  dirty: boolean;
  lastRebuildFrame: number;
  deps: unknown[][];
  cacheHit: boolean | null;
  cacheHitMiss: FiberCacheHitMiss;
  children: FiberNode[];
}

interface FiberDebugMetadata extends FiberNode {}

type ReconcilerDebugGlobal = typeof globalThis & {
  __fxeReconcilerSnapshot?: () => { tree: FiberNode[] };
};

let g_next_fiber_debug_id = 1;
const kFiberDebugMetadata = Symbol('fxe-ui.fiberDebugMetadata');

function summarizeDebugValue(value: unknown): unknown {
  if (value === null) return null;
  const kind = typeof value;
  if (kind === 'string' || kind === 'boolean') return value;
  if (kind === 'number') return Number.isFinite(value) ? value : 'number';
  if (kind === 'undefined' || kind === 'function' || kind === 'symbol' || kind === 'bigint') {
    return kind;
  }
  return 'object';
}

// Read lazily on first invocation so users can set globalThis.__FXE_DEV
// before mount() (which runs after their entry script's top-level code).
// Cached on first read; toggling later requires a reload.
let g_dev_mode_cached: boolean | undefined;
function devMode(): boolean {
  if (g_dev_mode_cached === undefined) {
    g_dev_mode_cached =
      typeof globalThis !== 'undefined' &&
      (globalThis as { __FXE_DEV?: boolean }).__FXE_DEV !== false;
  }
  return g_dev_mode_cached;
}

function summarizeProps(props: unknown): string {
  let summary: unknown;
  if (typeof props === 'object' && props !== null) {
    const out: Record<string, unknown> = {};
    for (const key of Object.keys(props as Record<string, unknown>)) {
      try {
        out[key] = summarizeDebugValue((props as Record<string, unknown>)[key]);
      } catch {
        out[key] = 'unreadable';
      }
    }
    summary = out;
  } else {
    summary = summarizeDebugValue(props);
  }
  try {
    return JSON.stringify(summary) ?? String(summary);
  } catch {
    return '"unserializable"';
  }
}

function summarizeDeps(deps: ReadonlyArray<unknown> | undefined): unknown[] {
  if (deps === undefined) return [];
  return deps.map((dep) => summarizeDebugValue(dep));
}

function depsForNode(node: Node): unknown[][] {
  if (node.type === 'layer')
    return node.props.deps === undefined ? [] : [summarizeDeps(node.props.deps)];
  if (node.type === 'draw')
    return node.props.deps === undefined ? [] : [summarizeDeps(node.props.deps)];
  return [];
}

function displayNameForNode(node: Node): string | null {
  if (node.type !== 'component') return null;
  return node.displayName ?? node.render.name ?? null;
}

function propsForNode(node: Node): unknown {
  if (node.type === 'component') return node.props;
  return node.props;
}

function ensureFiberDebugMetadata(fiber: Fiber): FiberDebugMetadata {
  let metadata = Reflect.get(fiber, kFiberDebugMetadata) as FiberDebugMetadata | undefined;
  if (!metadata) {
    metadata = {
      id: g_next_fiber_debug_id++,
      type: fiber.node?.type ?? 'unknown',
      displayName: fiber.node ? displayNameForNode(fiber.node) : null,
      key: fiber.key,
      props: '{}',
      propsSummary: '{}',
      dirty: fiber.dirty,
      lastRebuildFrame: 0,
      deps: [],
      cacheHit: null,
      cacheHitMiss: null,
      children: [],
    };
    Reflect.set(fiber, kFiberDebugMetadata, metadata);
  }
  return metadata;
}

// Fiber type/displayName are derived from `fiber.node` in reconcilerSnapshot().
// Keeping them out of the render hot path avoids touching devtools metadata
// for every node on every frame when no devtools snapshot is being read.
function recordFiberCacheStatus(
  fiber: Fiber,
  cacheHitMiss: FiberCacheHitMiss,
  rebuilt: boolean,
): void {
  if (!devMode()) return;
  const metadata = ensureFiberDebugMetadata(fiber);
  metadata.cacheHitMiss = cacheHitMiss;
  metadata.cacheHit = cacheHitMiss === null ? null : cacheHitMiss === 'hit';
  metadata.dirty = fiber.dirty;
  if (rebuilt) metadata.lastRebuildFrame = getTickFrameCounter();
}

function beginFiberHookDebugDeps(fiber: Fiber): void {
  if (!devMode()) return;
  ensureFiberDebugMetadata(fiber).deps = [];
}

function recordCurrentHookDebugDeps(deps: ReadonlyArray<unknown> | undefined): void {
  if (!devMode()) return;
  if (!g_ctx) return;
  ensureFiberDebugMetadata(g_ctx.fiber).deps.push(summarizeDeps(deps));
}

export function reconcilerSnapshot(): { tree: FiberNode[] } {
  const snapshotFiber = (fiber: Fiber): FiberNode | null => {
    if (!fiber.node) return null;
    const metadata = ensureFiberDebugMetadata(fiber);
    const children: FiberNode[] = [];
    for (const key of fiber.childOrder) {
      const child = fiber.children.get(key);
      const childSnapshot = child ? snapshotFiber(child) : null;
      if (childSnapshot) children.push(childSnapshot);
    }
    return {
      id: metadata.id,
      type: fiber.node.type,
      displayName: displayNameForNode(fiber.node),
      key: fiber.key,
      props: summarizeProps(propsForNode(fiber.node)),
      propsSummary: summarizeProps(propsForNode(fiber.node)),
      dirty: fiber.dirty,
      lastRebuildFrame: metadata.lastRebuildFrame,
      deps:
        fiber.node.type === 'component'
          ? metadata.deps.map((deps) => [...deps])
          : depsForNode(fiber.node),
      cacheHit: metadata.cacheHit,
      cacheHitMiss: metadata.cacheHitMiss,
      children,
    };
  };

  const tree: FiberNode[] = [];
  if (g_root) {
    for (const key of g_root.childOrder) {
      const child = g_root.children.get(key);
      const childSnapshot = child ? snapshotFiber(child) : null;
      if (childSnapshot) tree.push(childSnapshot);
    }
  }
  return { tree };
}

(globalThis as ReconcilerDebugGlobal).__fxeReconcilerSnapshot = reconcilerSnapshot;
installFiberTreeSnapshotProvider(reconcilerSnapshot as () => { tree: DevtoolsFiberNode[] });

function newFiber(key: string, parent: Fiber | null): Fiber {
  const fiber: Fiber = {
    key,
    node: null,
    parent,
    children: new Map(),
    childOrder: [],
    wrapperChildren: null,
    cache: null,
    cachedHitTargets: null,
    lastDeps: undefined,
    cacheAtlasEpoch: 0,
    lastProps: undefined,
    lastProducedNode: null,
    hooks: [],
    providedContext: null,
    providedValue: undefined,
    dirty: true,
    failed: false,
    surfaceHits: 0,
    surface: null,
    internalLayout: null,
    internalTextStyle: null,
    hasUnresolvedLayout: false,
  };
  const fiberId = ensureFiberDebugMetadata(fiber).id;
  registerFiberWork(fiberId, () => {
    markDirty(fiber);
    requestRenderTargetRedraw();
  });
  return fiber;
}

function markDirty(fiber: Fiber | null): void {
  // We cannot short-circuit on `f.dirty` here. Layer cache hits skip rendering
  // descendants entirely, which leaves their `dirty` flags set even after the
  // ancestor was rebuilt and cleared. If we then encountered a dirty descendant
  // and stopped early, the ancestors would never be re-marked, the next render
  // would cache-hit the ancestor again, and the descendant would never run —
  // so its setState would be silently dropped (focus, typing, etc. all break).
  // Walking the full chain to the root every time keeps invariants honest.
  let f: Fiber | null = fiber;
  while (f) {
    f.dirty = true;
    f = f.parent;
  }
}

function markLayoutUnresolved(fiber: Fiber | null): void {
  let f: Fiber | null = fiber;
  while (f) {
    g_layout_unresolved_during_render = true;
    f.hasUnresolvedLayout = true;
    f = f.parent;
  }
}

// Walk up from `fiber`, marking it and every ancestor dirty and layout-unsettled.
// The dirty bit requests traversal, while hasUnresolvedLayout survives currently
// rendering ancestors clearing their dirty bit as the stack unwinds.
export function markFiberAndAncestorsLayoutUnresolved(fiber: Fiber | null): void {
  markDirty(fiber);
  markLayoutUnresolved(fiber);
}

export function markFiberAndAncestorsDirty(fiber: Fiber | null): void {
  markDirty(fiber);
}

function scheduleFiberUpdate(fiber: Fiber): void {
  const lane = getCurrentSchedulerLane();
  if (lane === 'transition') {
    scheduleWork(ensureFiberDebugMetadata(fiber).id, 'transition');
    return;
  }
  markDirty(fiber);
  requestRenderTargetRedraw();
}

function fiberPath(fiber: Fiber): string {
  const parts: string[] = [];
  let f: Fiber | null = fiber;
  while (f) {
    parts.push(f.key);
    f = f.parent;
  }
  return parts.reverse().join('/');
}

// -------------------------------------------------------------- shallow deps

function depsEqual(
  a: ReadonlyArray<unknown> | undefined,
  b: ReadonlyArray<unknown> | undefined,
): boolean {
  if (a === undefined || b === undefined) return false; // undefined => always rebuild
  if (a === b) return true;
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; ++i) if (!Object.is(a[i], b[i])) return false;
  return true;
}

// ------------------------------------------------------------ render context

interface RenderCtx {
  fiber: Fiber;
  hookIndex: number;
  // Effects scheduled this render; flushed bottom-up after the tree walk.
  pendingEffects: { fiber: Fiber; slot: EffectSlot }[];
  pendingCleanups: Cleanup[];
  frameCallbacks: { fiber: Fiber; fn: (dtMs: number) => void }[];
  contextStack: ContextFrame[];
  parentTarget: CommandBuffer | Renderer;
  // Root renderer for this render pass. Surface caching needs it to
  // allocate user-texture slots and create offscreens that share the
  // device. null when the top-level target was a CommandBuffer (test
  // contexts), in which case surface caching is silently disabled.
  rootRenderer: Renderer | null;
}

let g_ctx: RenderCtx | null = null;
let g_context_frame_probe: readonly ContextFrameSnapshot[] | null = null;

export function currentContextFrames(): readonly ContextFrameSnapshot[] {
  return g_ctx?.contextStack ?? g_context_frame_probe ?? [];
}

export function getCurrentRenderFiber(): Fiber | null {
  return g_ctx?.fiber ?? null;
}

export function withContextFrames<T>(frames: readonly ContextFrameSnapshot[], fn: () => T): T {
  const prev = g_context_frame_probe;
  g_context_frame_probe = frames;
  try {
    return fn();
  } finally {
    g_context_frame_probe = prev;
  }
}

export function findChildFiberFor(parent: Fiber, child: Node, index: number): Fiber | undefined {
  return parent.children.get(childKey(child, index));
}

export function findVisibleChildFiber(parent: Fiber, child: Node, slotIndex: number): Fiber | null {
  return findVisibleByKey(parent, childKey(child, slotIndex));
}

function findVisibleByKey(parent: Fiber, targetKey: string): Fiber | null {
  const direct = parent.children.get(targetKey);
  if (direct) return direct;
  // No layer/provider/portal wrappers under this parent — the produced
  // subtree cannot be hiding behind one, so the miss is final. This is
  // the common case for plain View / Text / List children and avoids the
  // O(N) childOrder walk per missed lookup.
  const wrappers = parent.wrapperChildren;
  if (wrappers === null || wrappers.length === 0) return null;
  for (let i = 0; i < wrappers.length; ++i) {
    const found = findVisibleByKey(wrappers[i], targetKey);
    if (found) return found;
  }
  return null;
}

export function runInSandboxFiber<T>(fn: () => T): T {
  const prevCtx = requireCtx('runInSandboxFiber');
  const sandboxFiber: Fiber = {
    key: '__layout_sandbox__',
    node: null,
    parent: null,
    children: new Map(),
    childOrder: [],
    wrapperChildren: null,
    cache: null,
    lastDeps: undefined,
    cacheAtlasEpoch: 0,
    cachedHitTargets: null,
    surfaceHits: 0,
    surface: null,
    internalLayout: null,
    internalTextStyle: null,
    hooks: [],
    lastProps: undefined,
    lastProducedNode: null,
    providedContext: null,
    providedValue: undefined,
    dirty: true,
    failed: false,
    hasUnresolvedLayout: false,
  };
  g_ctx = {
    fiber: sandboxFiber,
    hookIndex: 0,
    pendingEffects: [],
    pendingCleanups: [],
    frameCallbacks: [],
    contextStack: [...prevCtx.contextStack],
    parentTarget: prevCtx.parentTarget,
    rootRenderer: prevCtx.rootRenderer,
  };
  try {
    return fn();
  } finally {
    g_ctx = prevCtx;
  }
}

function requireCtx(name: string): RenderCtx {
  if (!g_ctx) throw new Error(`${name}() called outside render`);
  return g_ctx;
}

function nextSlot<T extends BaseSlot>(kind: T['kind'], make: () => T): T {
  const ctx = requireCtx('hook');
  const idx = ctx.hookIndex++;
  const hooks = ctx.fiber.hooks;
  if (idx < hooks.length) {
    const existing = hooks[idx];
    if (existing.kind !== kind) {
      throw new Error(
        `rules-of-hooks: slot ${idx} was '${existing.kind}', now '${kind}'. ` +
          `Hooks must be called in the same order every render.`,
      );
    }
    return existing as T;
  }
  const fresh = make();
  hooks.push(fresh);
  return fresh;
}

// --------------------------------------------------------------------- hooks

export function useState<S>(initial: S): [S, (next: S | ((s: S) => S)) => void] {
  const ctx = requireCtx('useState');
  const fiber = ctx.fiber;
  const slot = nextSlot<StateSlot<S>>('state', () => {
    const s: StateSlot<S> = {
      kind: 'state',
      value: initial,
      setter: (next: S | ((p: S) => S)) => {
        if (getCurrentSchedulerLane() === 'transition' && !isTransitionFlushActive()) {
          scheduleCallback(() => s.setter(next), 'transition');
          return;
        }
        const prev = s.value;
        const resolved = typeof next === 'function' ? (next as (p: S) => S)(prev) : next;
        if (Object.is(prev, resolved)) return;
        s.value = resolved;
        scheduleFiberUpdate(fiber);
      },
    };
    return s;
  });
  return [slot.value, slot.setter];
}

export function useReducer<S, A>(
  reducer: (state: S, action: A) => S,
  initial: S,
): [S, (action: A) => void];
export function useReducer<S, A, I>(
  reducer: (state: S, action: A) => S,
  initial: I,
  init: (initial: I) => S,
): [S, (action: A) => void];
export function useReducer<S, A, I>(
  reducer: (state: S, action: A) => S,
  initial: S | I,
  init?: (initial: I) => S,
): [S, (action: A) => void] {
  const ctx = requireCtx('useReducer');
  const fiber = ctx.fiber;
  const slot = nextSlot<ReducerSlot<S, A>>('reducer', () => {
    const s: ReducerSlot<S, A> = {
      kind: 'reducer',
      value: init ? init(initial as I) : (initial as S),
      reducer,
      dispatch: (action: A) => {
        if (getCurrentSchedulerLane() === 'transition' && !isTransitionFlushActive()) {
          scheduleCallback(() => s.dispatch(action), 'transition');
          return;
        }
        const prev = s.value;
        const resolved = s.reducer(prev, action);
        if (Object.is(prev, resolved)) return;
        s.value = resolved;
        scheduleFiberUpdate(fiber);
      },
    };
    return s;
  });
  slot.reducer = reducer;
  return [slot.value, slot.dispatch];
}

export function useRef<T>(initial: T): { current: T } {
  const slot = nextSlot<RefSlot<T>>('ref', () => ({
    kind: 'ref',
    value: { current: initial },
  }));
  return slot.value;
}

installSchedulerHookApi({ useState, useRef });

export function useId(): string {
  const ctx = requireCtx('useId');
  const idx = ctx.hookIndex;
  const slot = nextSlot<IdSlot>('id', () => ({
    kind: 'id',
    value: `r:${fiberPath(ctx.fiber)}:${idx}`,
  }));
  return slot.value;
}

export function useContext<T>(context: Context<T>): T {
  const impl = context as ContextImpl<T>;
  const probeFrames = g_context_frame_probe;
  if (probeFrames !== null) {
    for (let i = probeFrames.length - 1; i >= 0; --i) {
      const frame = probeFrames[i];
      if (frame.ctx === impl) return frame.value as T;
    }
    return context.defaultValue;
  }
  const ctx = requireCtx('useContext');
  const slot = nextSlot<ContextSlot>('context', () => ({
    kind: 'context',
    ctx: impl as ContextImpl<unknown>,
  }));
  if (slot.ctx !== impl) {
    slot.ctx._consumers.delete(ctx.fiber);
    slot.ctx = impl as ContextImpl<unknown>;
  }
  impl._consumers.add(ctx.fiber);
  for (let i = ctx.contextStack.length - 1; i >= 0; --i) {
    const frame = ctx.contextStack[i];
    if (frame.ctx === impl) return frame.value as T;
  }
  return context.defaultValue;
}

// Framework-only readers for the parent-pushed layout / inherited text
// style. NOT counted toward rules-of-hooks (no slot consumption); they
// just expose the fiber's internalLayout / internalTextStyle fields to
// the running component body. Returns null when the parent didn't push
// any value (e.g. a top-level mount root before withRootLayout fires).
export function useInternalLayout(): LayoutResult | null {
  return g_ctx?.fiber.internalLayout ?? null;
}

export function useInternalTextStyle(): TextStyle | null {
  return g_ctx?.fiber.internalTextStyle ?? null;
}

export function useMemo<T>(fn: () => T, deps: ReadonlyArray<unknown>): T {
  const slot = nextSlot<MemoSlot<T>>('memo', () => ({
    kind: 'memo',
    deps,
    value: fn(),
  }));
  if (!depsEqual(slot.deps, deps)) {
    slot.value = fn();
    slot.deps = deps;
  }
  recordCurrentHookDebugDeps(deps);
  return slot.value;
}

export function useEffect(fn: () => undefined | Cleanup, deps?: ReadonlyArray<unknown>): void {
  const ctx = requireCtx('useEffect');
  const slot = nextSlot<EffectSlot>('effect', () => ({
    kind: 'effect',
    deps: undefined,
    cleanup: undefined,
    pending: null,
  }));
  const changed = deps === undefined || !depsEqual(slot.deps, deps);
  if (changed) {
    slot.pending = fn;
    slot.deps = deps;
    ctx.pendingEffects.push({ fiber: ctx.fiber, slot });
  }
  recordCurrentHookDebugDeps(deps);
}

export function useFrame(fn: (dtMs: number) => void): void {
  const ctx = requireCtx('useFrame');
  const slot = nextSlot<FrameSlot>('frame', () => ({ kind: 'frame', fn }));
  slot.fn = fn;
  ctx.frameCallbacks.push({ fiber: ctx.fiber, fn });
}

export function useEvent<K extends WindowEventName>(
  win: Window,
  kind: K,
  handler: (ev: WindowEventMap[K]) => void,
): void {
  requireCtx('useEvent');
  const handlerRef: { current: (ev: unknown) => void } = {
    current: handler as (ev: unknown) => void,
  };
  const slot = nextSlot<EventSlot>('event', () => {
    const dispose = win.on<K>(kind, (ev) => {
      handlerRef.current(ev);
    });
    return { kind: 'event', win, evt: kind, dispose, handlerRef };
  });
  // Always rebind the live handler so closures over fresh state work.
  slot.handlerRef.current = handler as (ev: unknown) => void;
  if (slot.win !== win || slot.evt !== kind) {
    slot.dispose();
    slot.win = win;
    slot.evt = kind;
    slot.dispose = win.on<K>(kind, (ev) => {
      slot.handlerRef.current(ev);
    });
  }
}

// ---------------------------------------------------- reconciliation walker

function childKey(node: Node, slotIndex: number): string {
  if (node.type === 'component' && node.key !== undefined) return `c:${node.key}`;
  if (node.type === 'layer' && node.props.key !== undefined) return `l:${node.props.key}`;
  if (node.type === 'draw' && node.key !== undefined) return `d:${node.key}`;
  if (node.type === 'provider' && node.key !== undefined) return `p:${node.key}`;
  if (node.type === 'portal' && node.key !== undefined) return `o:${node.key}`;
  if (node.type === 'error-boundary' && node.key !== undefined) return `e:${node.key}`;
  if (node.type === 'suspense' && node.key !== undefined) return `s:${node.key}`;
  return `#${slotIndex}:${node.type}`;
}

function isDevMode(): boolean {
  return (
    typeof globalThis !== 'undefined' && (globalThis as { __FXE_DEV?: boolean }).__FXE_DEV !== false
  );
}

function duplicateKeyError(key: string, parent: Fiber): Error {
  return new Error(`fxe-ui: duplicate key "${key}" at ${fiberPath(parent)}`);
}

function isDuplicateKeyDiagnostic(error: unknown): boolean {
  return error instanceof Error && error.message.startsWith('fxe-ui: duplicate key ');
}

function sameNodeIdentity(prev: Node | null, next: Node): boolean {
  if (!prev) return true;
  if (prev.type !== next.type) return false;
  if (prev.type === 'component' && next.type === 'component') {
    return (prev.componentType ?? prev.render) === (next.componentType ?? next.render);
  }
  if (prev.type === 'provider' && next.type === 'provider') {
    return prev.props.ctx === next.props.ctx;
  }
  return true;
}

function isWrapperNode(node: Node): boolean {
  return node.type === 'layer' || node.type === 'provider' || node.type === 'portal';
}

function reconcileSingleChild(parent: Fiber, node: Node): Fiber {
  const k = childKey(node, 0);
  let f = parent.children.get(k);
  if (f && !sameNodeIdentity(f.node, node)) {
    unmountFiber(f);
    f = newFiber(k, parent);
    parent.children.set(k, f);
  }
  if (!f) {
    f = newFiber(k, parent);
    parent.children.set(k, f);
  }
  for (const [existingKey, child] of parent.children) {
    if (existingKey === k) continue;
    unmountFiber(child);
    parent.children.delete(existingKey);
  }
  if (parent.childOrder.length !== 1 || parent.childOrder[0] !== k) {
    parent.childOrder = [k];
  }
  if (isWrapperNode(node)) {
    if (
      parent.wrapperChildren === null ||
      parent.wrapperChildren.length !== 1 ||
      parent.wrapperChildren[0] !== f
    ) {
      parent.wrapperChildren = [f];
    }
  } else {
    parent.wrapperChildren = null;
  }
  return f;
}

function reconcileChildren(parent: Fiber, nodes: readonly Node[]): Fiber[] {
  const liveKeys = new Set<string>();
  const ordered: Fiber[] = [];
  const order: string[] = [];
  let wrappers: Fiber[] | null = null;
  for (let i = 0; i < nodes.length; ++i) {
    const node = nodes[i];
    const k = childKey(node, i);
    if (isDevMode() && liveKeys.has(k)) throw duplicateKeyError(k, parent);
    liveKeys.add(k);
    let f = parent.children.get(k);
    if (f && !sameNodeIdentity(f.node, node)) {
      unmountFiber(f);
      f = newFiber(k, parent);
      parent.children.set(k, f);
    }
    if (!f) {
      f = newFiber(k, parent);
      parent.children.set(k, f);
    }
    ordered.push(f);
    order.push(k);
    if (isWrapperNode(node)) {
      if (wrappers === null) wrappers = [];
      wrappers.push(f);
    }
  }
  // Drop fibers that disappeared this frame; run their cleanups.
  for (const [k, f] of parent.children) {
    if (!liveKeys.has(k)) {
      unmountFiber(f);
      parent.children.delete(k);
    }
  }
  parent.childOrder = order;
  parent.wrapperChildren = wrappers;
  return ordered;
}

function unmountFiber(f: Fiber): void {
  // Children first — child cleanups run before parent cleanups.
  for (const c of f.children.values()) unmountFiber(c);
  for (const slot of f.hooks) {
    if (slot.kind === 'effect') {
      const eff = slot as EffectSlot;
      if (typeof eff.cleanup === 'function') {
        try {
          eff.cleanup();
        } catch (e) {
          console.error(`useEffect cleanup threw on unmount: ${e}`);
        }
      }
    } else if (slot.kind === 'event') {
      const evt = slot as EventSlot;
      try {
        evt.dispose();
      } catch (e) {
        console.error(`useEvent dispose threw on unmount: ${e}`);
      }
    } else if (slot.kind === 'context') {
      const context = slot as ContextSlot;
      context.ctx._consumers.delete(f);
    }
  }
  f.children.clear();
  f.hooks = [];
  f.cache = null;
  if (f.surface !== null) {
    releaseSurface(f.surface);
    f.surface = null;
  }
  f.lastProps = undefined;
  f.lastProducedNode = null;
  f.internalLayout = null;
  f.internalTextStyle = null;
  f.providedContext = null;
  f.providedValue = undefined;
  const fiberId = ensureFiberDebugMetadata(f).id;
  unregisterFiberSignalSubscriptions(fiberId);
  unregisterFiberWork(fiberId);
}

type Thenable = { then: (onFulfilled: () => void, onRejected: () => void) => unknown };

const g_suspense_wakeables = new WeakSet<object>();

function asThenable(value: unknown): Thenable | null {
  if ((typeof value !== 'object' && typeof value !== 'function') || value === null) return null;
  try {
    const then = (value as { then?: unknown }).then;
    return typeof then === 'function' ? (value as unknown as Thenable) : null;
  } catch {
    return null;
  }
}

export function requestRenderTargetRedraw(): void {
  const win = getRenderTarget();
  if (win) win.requestRedraw();
}

function watchSuspenseWakeable(value: unknown): void {
  const thenable = asThenable(value);
  if (!thenable) return;
  if (typeof value === 'object' && value !== null) {
    if (g_suspense_wakeables.has(value)) return;
    g_suspense_wakeables.add(value);
  }
  thenable.then(requestRenderTargetRedraw, requestRenderTargetRedraw);
}

function renderNodeList(
  owner: Fiber,
  nodes: readonly Node[],
  target: CommandBuffer | Renderer,
  ctx: RenderCtx,
  forceTraversal = false,
): void {
  if (nodes.length === 1) {
    const node = nodes[0];
    renderNode(node, reconcileSingleChild(owner, node), target, ctx, forceTraversal);
    return;
  }
  const children = reconcileChildren(owner, nodes);
  for (let i = 0; i < nodes.length; ++i) {
    renderNode(nodes[i], children[i], target, ctx, forceTraversal);
  }
}

function renderBoundaryFallback(
  fiber: Fiber,
  child: BoundaryChild,
  target: CommandBuffer | Renderer,
  ctx: RenderCtx,
  forceTraversal = false,
): void {
  const fallback = singleBoundaryNode(child);
  renderNodeList(fiber, [fallback], target, ctx, forceTraversal);
}

function renderNode(
  node: Node,
  fiber: Fiber,
  target: CommandBuffer | Renderer,
  ctx: RenderCtx,
  forceTraversal = false,
): void {
  fiber.node = node;
  if (node.type === 'draw') {
    recordFiberCacheStatus(fiber, 'miss', true);
    fiber.dirty = false;
    try {
      node.props.fn(target as CommandBuffer);
    } catch (e) {
      fiber.failed = true;
      throw e;
    }
    return;
  }
  if (node.type === 'component') {
    const memoInfo = node.memo;
    const epoch = memoInfo ? atlasEpoch() : 0;
    const wasDirty = fiber.dirty;
    const hadUnresolvedLayout = fiber.hasUnresolvedLayout;
    const nextInternalLayout = node.internalLayout ?? null;
    const nextInternalTextStyle = node.internalTextStyle ?? null;
    const internalInputsEqual =
      layoutResultEqual(fiber.internalLayout, nextInternalLayout) &&
      shallowEqualProps(fiber.internalTextStyle, nextInternalTextStyle);
    // React-style pendingProps swap. If props are shallow-equal to last
    // render, point this render's node at the previous props object so any
    // identity-keyed cache downstream (layout, useMemo deps over parent ref,
    // etc.) hits. Memo'd nodes use their custom comparator; everything else
    // pays one shallowEqualProps call per render.
    const hadLastProps = fiber.lastProps !== undefined;
    let propsEqualToLast = hadLastProps && Object.is(fiber.lastProps, node.props);
    if (hadLastProps && !propsEqualToLast) {
      propsEqualToLast =
        node.memo !== undefined
          ? node.memo.areEqual(fiber.lastProps, node.props)
          : shallowEqualProps(fiber.lastProps, node.props);
      if (propsEqualToLast) {
        (node as { props: unknown }).props = fiber.lastProps;
      }
    }
    // If this component's output may have changed, wrapper children below it
    // must not replay stale layer caches before changed descendant props land.
    const forceProducedTraversal =
      hadUnresolvedLayout || wasDirty || !internalInputsEqual || !hadLastProps || !propsEqualToLast;
    if (memoInfo) {
      const trace = memoTraceState();
      const bail =
        internalInputsEqual &&
        !wasDirty &&
        !hadUnresolvedLayout &&
        fiber.cache !== null &&
        hadLastProps &&
        fiber.cacheAtlasEpoch === epoch &&
        propsEqualToLast;
      if (trace) {
        const dn = node.displayName ?? 'anon';
        let slot = trace.byName.get(dn);
        if (!slot) {
          slot = {
            total: 0,
            dirty: 0,
            layout: 0,
            noCache: 0,
            noLastProps: 0,
            epoch: 0,
            propsDiff: 0,
            hit: 0,
          };
          trace.byName.set(dn, slot);
        }
        const reason: keyof typeof slot = wasDirty
          ? 'dirty'
          : hadUnresolvedLayout
            ? 'layout'
            : fiber.cache === null
              ? 'noCache'
              : !hadLastProps
                ? 'noLastProps'
                : fiber.cacheAtlasEpoch !== epoch
                  ? 'epoch'
                  : !propsEqualToLast
                    ? 'propsDiff'
                    : 'hit';
        trace.totals.total++;
        slot.total++;
        trace.totals[reason]++;
        slot[reason]++;
        if (reason === 'propsDiff' && !trace.propsDump.has(dn)) {
          const last = fiber.lastProps;
          const next = node.props;
          trace.propsDump.set(dn, {
            last,
            next,
            lastKeys: typeof last === 'object' && last !== null ? Object.keys(last as object) : [],
            nextKeys: typeof next === 'object' && next !== null ? Object.keys(next as object) : [],
          });
        }
      }
      if (bail) {
        const produced = fiber.lastProducedNode;
        if (produced && shouldTraverseMemoProduced(produced)) {
          recordFiberCacheStatus(fiber, 'hit', false);
          RenderStats.recordCacheHit();
          renderNodeList(fiber, [produced], target, ctx, false);
          fiber.dirty = false;
          return;
        }
        recordFiberCacheStatus(fiber, 'hit', false);
        queueInto(target, fiber.cache as CommandBuffer, undefined, undefined);
        if (fiber.cachedHitTargets && fiber.cachedHitTargets.length > 0) {
          replayHitTargets(fiber.cachedHitTargets);
        }
        RenderStats.recordCacheHit();
        return;
      }
    }
    fiber.layoutCache = undefined; // body re-ran; any derived layout is stale
    fiber.hasUnresolvedLayout = false;

    const prevCtx = g_ctx;
    const myCtx: RenderCtx = {
      fiber,
      hookIndex: 0,
      pendingEffects: ctx.pendingEffects,
      pendingCleanups: ctx.pendingCleanups,
      frameCallbacks: ctx.frameCallbacks,
      contextStack: ctx.contextStack,
      parentTarget: target,
      rootRenderer: ctx.rootRenderer,
    };
    g_ctx = myCtx;
    beginFiberHookDebugDeps(fiber);
    const prevSignalTracking = beginFiberSignalTracking(ensureFiberDebugMetadata(fiber).id);
    let produced: Node | null = null;
    let thrown: unknown = null;
    // Push framework-internal state onto the fiber so the body's
    // useInternalLayout/useInternalTextStyle hooks see the parent's
    // commit values for this render.
    fiber.internalLayout = nextInternalLayout;
    fiber.internalTextStyle = nextInternalTextStyle;
    try {
      produced = node.render(node.props);
    } catch (e) {
      fiber.failed = true;
      thrown = e;
    } finally {
      if (thrown === null && myCtx.hookIndex < fiber.hooks.length) {
        // A reduction in hook count is itself a rules-of-hooks violation.
        thrown = new Error(
          `rules-of-hooks: component called ${myCtx.hookIndex} hooks but had ${fiber.hooks.length}`,
        );
      }
      g_ctx = prevCtx;
      endFiberSignalTracking(prevSignalTracking);
    }
    if (thrown !== null) throw thrown;
    produced = propagateInternalToProduced(node, produced);
    fiber.lastProducedNode = produced;
    if (!produced) {
      fiber.cache = memoInfo ? new CommandBuffer() : fiber.cache;
      if (memoInfo) {
        fiber.cachedHitTargets = null;
      }
      fiber.lastProps = node.props;
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
      return;
    }
    // Wrap the produced node as the single child of this fiber.
    if (memoInfo) {
      recordFiberCacheStatus(fiber, 'miss', true);
      RenderStats.recordCacheMiss();
      RenderStats.recordRebuild();
      if (shouldTraverseMemoProduced(produced)) {
        const empty = new CommandBuffer();
        fiber.cachedHitTargets = null;
        fiber.cache = empty;
        fiber.cacheAtlasEpoch = epoch;
        renderNodeList(fiber, [produced], target, ctx, forceProducedTraversal);
        fiber.lastProps = node.props;
        fiber.dirty = false;
      } else {
        const fresh = new CommandBuffer();
        const hitStart = hitTargetCount();
        renderNodeList(fiber, [produced], fresh, ctx, forceProducedTraversal);
        fiber.cachedHitTargets = captureHitTargetsSince(hitStart);
        fiber.cache = fresh;
        fiber.cacheAtlasEpoch = epoch;
        fiber.lastProps = node.props;
        fiber.dirty = false;
        queueInto(target, fresh, undefined, undefined);
      }
    } else {
      renderNodeList(fiber, [produced], target, ctx, forceProducedTraversal);
      fiber.lastProps = node.props;
      recordFiberCacheStatus(fiber, 'miss', true);
      fiber.dirty = false;
    }
    return;
  }
  if (node.type === 'provider') {
    const providerCtx = node.props.ctx as ContextImpl<unknown>;
    // Use shallow-equal (not Object.is): callers like View build a fresh
    // textStyle object every render via spread (`{...inherited, ...resolved}`)
    // so two structurally-identical values would still fail Object.is and
    // force a markDirty walk over every consumer fiber. In a stress scene
    // (1800 Text consumers × full root walk per frame) that previously
    // dominated the profile at ~37%. shallowEqual catches the common case
    // and falls back to inequality for primitives / different shapes.
    // A provider mounting for the first time must not dirty the context's
    // global consumer set: those consumers belong to other provider scopes,
    // while this provider's own children read the pushed value below.
    const hadProvidedContext = fiber.providedContext !== null;
    const valueChanged =
      hadProvidedContext &&
      (fiber.providedContext !== providerCtx ||
        !shallowEqualProps(fiber.providedValue, node.props.value));
    fiber.providedContext = providerCtx;
    fiber.providedValue = node.props.value;
    if (valueChanged) {
      for (const consumer of providerCtx._consumers) markDirty(consumer);
    }
    fiber.hasUnresolvedLayout = false;
    ctx.contextStack.push({ ctx: providerCtx, value: node.props.value });
    try {
      renderNodeList(
        fiber,
        normalizeBoundaryChildren(node.props.children),
        target,
        ctx,
        forceTraversal,
      );
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
    } finally {
      ctx.contextStack.pop();
    }
    return;
  }
  if (node.type === 'portal') {
    fiber.hasUnresolvedLayout = false;
    renderNodeList(
      fiber,
      normalizeBoundaryChildren(node.props.children),
      node.props.to,
      ctx,
      forceTraversal,
    );
    recordFiberCacheStatus(fiber, 'miss', true);
    fiber.dirty = false;
    return;
  }
  if (node.type === 'error-boundary') {
    fiber.hasUnresolvedLayout = false;
    const fresh = new CommandBuffer();
    const children = normalizeBoundaryChildren(node.props.children);
    try {
      renderNodeList(fiber, children, fresh, ctx, forceTraversal);
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
      queueInto(target, fresh, undefined, undefined);
    } catch (e) {
      if (asThenable(e)) throw e;
      if (isDuplicateKeyDiagnostic(e)) throw e;
      fiber.failed = true;
      node.props.onError?.(e);
      const fallback =
        typeof node.props.fallback === 'function' ? node.props.fallback(e) : node.props.fallback;
      renderBoundaryFallback(fiber, fallback, target, ctx, forceTraversal);
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
    }
    return;
  }
  if (node.type === 'suspense') {
    fiber.hasUnresolvedLayout = false;
    const fresh = new CommandBuffer();
    const children = normalizeBoundaryChildren(node.props.children);
    try {
      renderNodeList(fiber, children, fresh, ctx, forceTraversal);
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
      queueInto(target, fresh, undefined, undefined);
    } catch (e) {
      if (!asThenable(e)) throw e;
      watchSuspenseWakeable(e);
      renderBoundaryFallback(fiber, node.props.fallback, target, ctx, forceTraversal);
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
    }
    return;
  }
  // Layer.
  if (forceTraversal) fiber.dirty = true;
  const props = node.props;
  const wantDeps = props.deps;
  const layerEpoch = atlasEpoch();
  const cacheable =
    wantDeps !== undefined &&
    !fiber.dirty &&
    !fiber.hasUnresolvedLayout &&
    fiber.cache !== null &&
    fiber.cacheAtlasEpoch === layerEpoch &&
    depsEqual(fiber.lastDeps, wantDeps);

  if (cacheable && fiber.cache) {
    const cached = fiber.cache;
    // Surface cache fast path. If we baked this fiber on a previous frame
    // and its cache hasn't changed underneath us, draw a single textured
    // quad sampling the baked surface instead of replaying the vertex
    // stream. Falls back to the regular replay if the bake was discarded
    // (slot freed, parent transform mismatch, etc.).
    if (
      !SURFACE_CACHE_DISABLED &&
      fiber.surface !== null &&
      fiber.surface.bakedEpoch === cached.__fxe_epoch &&
      target instanceof Renderer
    ) {
      const s = fiber.surface;
      // Per-Layer transform/tint propagate through the textured quad's
      // tint channel; the rect itself is in absolute coords so user
      // transforms are applied via the standard target.queue() with the
      // surface emitting through the renderer's primitive pipeline. For
      // simplicity we ignore props.transform/tint here — surface caching
      // is opt-out for transformed/tinted layers (covered by the
      // !shouldSurfaceCache check below).
      Primitives.drawTextureQuad(target, s.slot, s.x, s.y, s.width, s.height);
      if (fiber.cachedHitTargets && fiber.cachedHitTargets.length > 0) {
        replayHitTargets(fiber.cachedHitTargets);
      }
      RenderStats.recordCacheHit();
      recordFiberCacheStatus(fiber, 'hit', false);
      return;
    }
    queueInto(target, cached, props.transform, props.tint);
    if (fiber.cachedHitTargets && fiber.cachedHitTargets.length > 0) {
      replayHitTargets(fiber.cachedHitTargets);
    }
    RenderStats.recordCacheHit();
    recordFiberCacheStatus(fiber, 'hit', false);
    // Stability counter — kicks the bake heuristic. We only bake when the
    // layer is "expensive enough" (vertex count above the threshold) AND
    // not already running a transform/tint (those would need per-frame
    // re-bake, defeating the purpose).
    if (
      !SURFACE_CACHE_DISABLED &&
      fiber.surface === null &&
      ctx.rootRenderer !== null &&
      props.transform === undefined &&
      props.tint === undefined &&
      cached.__fxe_v_len >= SURFACE_MIN_VERTS
    ) {
      fiber.surfaceHits++;
      if (fiber.surfaceHits >= SURFACE_BAKE_HITS) {
        const baked = bakeFiberSurface(cached, ctx.rootRenderer);
        if (baked !== null) fiber.surface = baked;
        // Reset so we don't re-attempt every frame on a slot-cap miss.
        fiber.surfaceHits = 0;
      }
    }
    return;
  }

  // Rebuild path.
  // Cache invalidated → any baked surface is stale. Free its slot back to
  // the renderer pool so other fibers can claim it.
  if (fiber.surface !== null) {
    releaseSurface(fiber.surface);
    fiber.surface = null;
  }
  fiber.surfaceHits = 0;
  RenderStats.recordRebuild();
  RenderStats.recordCacheMiss();
  recordFiberCacheStatus(fiber, 'miss', true);
  fiber.hasUnresolvedLayout = false;
  const hitStart = hitTargetCount();
  const fresh = new CommandBuffer();
  renderNodeList(fiber, props.children, fresh, ctx, forceTraversal);
  fiber.cache = fresh;
  fiber.cacheAtlasEpoch = layerEpoch;
  fiber.lastDeps = wantDeps;
  fiber.cachedHitTargets = captureHitTargetsSince(hitStart);
  fiber.dirty = false;
  queueInto(target, fresh, props.transform, props.tint);
  queuePaintFlashOutline(target, fresh, props.transform);
}

function queueInto(
  target: CommandBuffer | Renderer,
  src: CommandBuffer,
  transform: Mat4 | undefined,
  tint: Vec4 | undefined,
): void {
  if (src.__fxe_v_len === 0) return;
  if (transform && tint) target.queue(src, transform, tint);
  else if (transform) target.queue(src, transform);
  else target.queue(src);
}

function queuePaintFlashOutline(
  target: CommandBuffer | Renderer,
  src: CommandBuffer,
  transform: Mat4 | undefined,
): void {
  if (!isPaintFlashEnabled() || src.__fxe_v_len === 0) return;
  const vertices = src.vertexBuffer();
  if (vertices.length < 2) return;
  let minX = Number.POSITIVE_INFINITY;
  let minY = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY;
  let maxY = Number.NEGATIVE_INFINITY;
  for (let i = 0; i + 1 < vertices.length; i += 8) {
    const x = vertices[i];
    const y = vertices[i + 1];
    if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
    minX = Math.min(minX, x);
    minY = Math.min(minY, y);
    maxX = Math.max(maxX, x);
    maxY = Math.max(maxY, y);
  }
  if (!(maxX > minX && maxY > minY)) return;
  const outline = new CommandBuffer();
  Primitives.drawRect(outline, minX, minY, maxX - minX, maxY - minY, 0, 0xff00ffff, 1);
  if (transform) target.queue(outline, transform);
  else target.queue(outline);
}

// ----------------------------------------------------------------- entry

let g_root: Fiber | null = null;
let g_layout_unresolved_during_render = false;

export function consumeLayoutUnresolvedDuringRender(): boolean {
  const out = g_layout_unresolved_during_render;
  g_layout_unresolved_during_render = false;
  return out;
}

export interface RenderOptions {
  animate?: boolean;
  frameLoop?: FrameLoopOptions;
}

export function render(
  root: Node,
  target: CommandBuffer | Renderer,
  options?: RenderOptions,
): void {
  g_layout_unresolved_during_render = false;
  if (!g_root) g_root = newFiber('$root', null);
  const ctx: RenderCtx = {
    fiber: g_root,
    hookIndex: 0,
    pendingEffects: [],
    pendingCleanups: [],
    frameCallbacks: [],
    contextStack: [],
    parentTarget: target,
    // Top-level Renderer is the surface-cache root. Test contexts and
    // nested portal renders that pass a CommandBuffer get null and
    // surface caching no-ops.
    rootRenderer: target instanceof Renderer ? target : null,
  };
  // Top-level slot is a single child under $root.
  const children = reconcileChildren(g_root, [root]);
  let completed = false;
  try {
    renderNode(root, children[0], target, ctx);
    completed = true;
  } catch (e) {
    if (isDuplicateKeyDiagnostic(e)) throw e;
    if (asThenable(e)) {
      watchSuspenseWakeable(e);
      console.error('render suspended without <Suspense>');
    } else {
      console.error(`render failed: ${e}`);
    }
  }

  if (!completed) return;
  // Flush effects: child cleanups before parent cleanups, then run new
  // effects from child up to parent (matches React commit phase ordering).
  // pendingEffects was populated in render order (parent before child), so
  // reverse for cleanup+run.
  for (let i = ctx.pendingEffects.length - 1; i >= 0; --i) {
    const { slot } = ctx.pendingEffects[i];
    if (typeof slot.cleanup === 'function') {
      try {
        slot.cleanup();
      } catch (e) {
        console.error(`useEffect cleanup threw: ${e}`);
      }
    }
    slot.cleanup = undefined;
  }
  for (let i = ctx.pendingEffects.length - 1; i >= 0; --i) {
    const { slot } = ctx.pendingEffects[i];
    const fn = slot.pending;
    slot.pending = null;
    if (!fn) continue;
    try {
      const cleanup = fn();
      slot.cleanup = typeof cleanup === 'function' ? cleanup : undefined;
    } catch (e) {
      console.error(`useEffect threw: ${e}`);
    }
  }
  // Refresh the frame-callback registry from this render's collected hooks.
  g_frame_callbacks.length = 0;
  for (const { fn } of ctx.frameCallbacks) g_frame_callbacks.push(fn);
  if (options?.animate === true) ensureRenderFrameLoop(options.frameLoop);
}
