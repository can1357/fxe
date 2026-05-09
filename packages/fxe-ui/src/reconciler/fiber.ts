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

import type { Mat4, Vec4, Window, WindowDisposer, WindowEventMap, WindowEventName } from 'fxe';
import { CommandBuffer, OffscreenRenderer, Primitives, Renderer } from 'fxe';
import { tickAnimatedFrames } from '../animated/timing.ts';
import {
  captureHitTargetsSince,
  type HitTarget,
  hitTargetCount,
  replayHitTargets,
} from '../mount/hit_test.ts';
import {
  type DevtoolsFiberNode,
  installFiberTreeSnapshotProvider,
  isPaintFlashEnabled,
} from './devtools.ts';
import {
  getCurrentSchedulerLane,
  installSchedulerHookApi,
  isTransitionFlushActive,
  registerFiberWork,
  scheduleCallback,
  scheduleWork,
  tickSchedulerFrame,
  unregisterFiberWork,
} from './scheduler.ts';
import {
  beginFiberSignalTracking,
  endFiberSignalTracking,
  unregisterFiberSignalSubscriptions,
} from './signals.ts';

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

// ----------------------------------------------------- Surface caching
//
// When a Layer's cache survives unchanged for SURFACE_BAKE_HITS frames AND
// its rasterized vertex count clears SURFACE_MIN_VERTS, we transparently
// bake the cached subtree into an offscreen GPU texture and replace future
// cache replays with a single drawTextureQuad. This collapses N vertex
// uploads + N glyph samples per frame into 4 verts + one bilinear sample.
//
// Slot count is capped at 4 (matches the WGSL `user_tex_*` bindings).
// Beyond 4 stable expensive subtrees we simply skip baking the rest;
// already-baked surfaces continue to replay normally.
const SURFACE_BAKE_HITS = 4;
const SURFACE_MIN_VERTS = 200;
const SURFACE_SLOT_CAP = 4;
const SURFACE_PAD = 1;

// Disable surface caching when FXE_DISABLE_SURFACE_CACHE=1 (benchmark unbaked
// path or work around cache bugs without rebuilding).
const SURFACE_CACHE_DISABLED = process.env.FXE_DISABLE_SURFACE_CACHE === '1';

// Glyph atlas generation snapshot. Bumped whenever the shared glyph cache
// repacks (LRU eviction). Cached vertex data records the epoch it was
// built against; if the live atlas has advanced since, replaying would
// sample stale UVs and render scrambled glyphs, so we treat any mismatch
// as a cache miss and rebuild. `Primitives.atlasEpoch` is a tiny native
// hop returning a Number, safe to call once per cached fiber per frame.
function atlasEpoch(): number {
  return Primitives.atlasEpoch();
}

interface SurfaceCacheEntry {
  off: OffscreenRenderer;
  slot: number;
  x: number;
  y: number;
  width: number;
  height: number;
  renderer: Renderer;
  bakedEpoch: number;
  // Atlas generation observed at bake time. The bake itself produced the
  // offscreen pixels under that atlas layout, so the offscreen texture
  // contents are stable; we only need to invalidate when the *upstream*
  // command buffer that fed the bake has been re-cached against a newer
  // atlas (which forces a fresh bake anyway via cache rebuild). The field
  // is here for symmetry with cacheAtlasEpoch + future cross-frame
  // checking; the surface fast path itself does not gate on it.
  atlasEpoch: number;
}

interface RendererSurfaceState {
  free: number[];
}
const kRendererSurfaceState = Symbol('fxe-ui.rendererSurfaceState');

function rendererSurfaceState(r: Renderer): RendererSurfaceState {
  let s = Reflect.get(r, kRendererSurfaceState) as RendererSurfaceState | undefined;
  if (!s) {
    s = { free: [0, 1, 2, 3].slice(0, SURFACE_SLOT_CAP) };
    Reflect.set(r, kRendererSurfaceState, s);
  }
  return s;
}

function allocSurfaceSlot(r: Renderer): number | null {
  const s = rendererSurfaceState(r);
  return s.free.length > 0 ? (s.free.shift() as number) : null;
}

function freeSurfaceSlot(r: Renderer, slot: number): void {
  const s = rendererSurfaceState(r);
  if (!s.free.includes(slot)) s.free.push(slot);
}

function translateMat(tx: number, ty: number): Float32Array {
  const m = new Float32Array(16);
  m[0] = 1;
  m[5] = 1;
  m[10] = 1;
  m[15] = 1;
  m[12] = tx;
  m[13] = ty;
  return m;
}

function bakeFiberSurface(cache: CommandBuffer, renderer: Renderer): SurfaceCacheEntry | null {
  const bb = cache.bounds();
  if (!bb || bb.width <= 0 || bb.height <= 0) return null;
  const slot = allocSurfaceSlot(renderer);
  if (slot === null) return null;
  const pad = SURFACE_PAD;
  const w = Math.max(1, Math.ceil(bb.width + pad * 2));
  const h = Math.max(1, Math.ceil(bb.height + pad * 2));
  let off: OffscreenRenderer;
  try {
    off = new OffscreenRenderer({
      width: w,
      height: h,
      parent: renderer,
    });
  } catch (_e) {
    freeSurfaceSlot(renderer, slot);
    return null;
  }
  off.beginFrame();
  off.setClearColor(0, 0, 0, 0);
  off.queue(cache, translateMat(-bb.x + pad, -bb.y + pad));
  off.endFrame();
  renderer.bindUserTexture(slot, off);
  return {
    off,
    slot,
    x: bb.x - pad,
    y: bb.y - pad,
    width: w,
    height: h,
    renderer,
    bakedEpoch: cache.epoch(),
    atlasEpoch: atlasEpoch(),
  };
}

function releaseSurface(entry: SurfaceCacheEntry): void {
  try {
    entry.renderer.bindUserTexture(entry.slot, null);
  } catch (_e) {
    /* renderer may already be torn down */
  }
  freeSurfaceSlot(entry.renderer, entry.slot);
}

export function Component<P>(
  render: (props: P) => Node,
  displayName?: string,
): (props: P & { key?: string }) => Node {
  const componentType: ComponentIdentity = {};
  const factory = (props: P & { key?: string }): Node => ({
    type: 'component',
    componentType,
    render: (raw: unknown) => render(raw as P),
    props,
    displayName,
    key: props.key,
  });
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

function shallowEqualProps(prev: unknown, next: unknown): boolean {
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

export function memo<P>(
  component: (props: P & { key?: string }) => Node,
  areEqual?: PropsEqual<P & { key?: string }>,
): (props: P & { key?: string }) => Node {
  const compare = (areEqual ?? shallowEqualProps) as (prev: unknown, next: unknown) => boolean;
  return (props: P & { key?: string }) => {
    const node = component(props);
    if (node.type !== 'component') {
      throw new TypeError('memo() expects a fxe-ui Component');
    }
    return { ...node, memo: { areEqual: compare } };
  };
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

function propagateInternalComponentProps(node: Node, produced: Node | null): Node | null {
  if (produced === null) return produced;
  if (node.type !== 'component') return produced;
  if (produced.type !== 'component') return produced;
  const props = node.props as { __layout?: unknown; __textStyle?: unknown };
  if (props.__layout === undefined && props.__textStyle === undefined) return produced;
  return {
    ...produced,
    props: {
      ...(produced.props as Record<string, unknown>),
      ...(props.__layout === undefined ? {} : { __layout: props.__layout }),
      ...(props.__textStyle === undefined ? {} : { __textStyle: props.__textStyle }),
    },
  };
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
  cleanup: Cleanup | void;
  pending: (() => void | Cleanup) | null;
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

export type FrameLoopDisposer = () => void;

export interface FrameLoopOptions {
  requestAnimationFrame?: (fn: (timeMs: number) => void) => unknown;
  cancelAnimationFrame?: (id: unknown) => void;
}

type FxeUiFrameLoopBridgeGlobal = typeof globalThis & {
  __fxeUiEnsureFrameLoop?: () => FrameLoopDisposer;
};

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

interface Fiber {
  key: string; // structural identity in the parent's child list
  node: Node | null; // last node we reconciled into this fiber
  parent: Fiber | null;
  children: Map<string, Fiber>; // keyed lookup (insertion order = render order)
  childOrder: string[];
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
  // Component-only:
  hooks: HookSlot[];
  lastProps: unknown;
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
let g_tick_frame_counter = 0;
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
      props: fiber.node ? summarizeProps(propsForNode(fiber.node)) : '{}',
      propsSummary: fiber.node ? summarizeProps(propsForNode(fiber.node)) : '{}',
      dirty: fiber.dirty,
      lastRebuildFrame: 0,
      deps: fiber.node ? depsForNode(fiber.node) : [],
      cacheHit: null,
      cacheHitMiss: null,
      children: [],
    };
    Reflect.set(fiber, kFiberDebugMetadata, metadata);
  }
  return metadata;
}

function updateFiberDebugNode(fiber: Fiber, node: Node): void {
  if (!devMode()) return; // skip in production: summarize + stringify per render
  const metadata = ensureFiberDebugMetadata(fiber);
  metadata.type = node.type;
  metadata.displayName = displayNameForNode(node);
  metadata.props = summarizeProps(propsForNode(node));
  metadata.propsSummary = metadata.props;
  if (node.type !== 'component') metadata.deps = depsForNode(node);
}
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
  if (rebuilt) metadata.lastRebuildFrame = g_tick_frame_counter;
}

function beginFiberHookDebugDeps(fiber: Fiber): void {
  ensureFiberDebugMetadata(fiber).deps = [];
}

function recordCurrentHookDebugDeps(deps: ReadonlyArray<unknown> | undefined): void {
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
      type: metadata.type,
      displayName: metadata.displayName,
      key: fiber.key,
      props: metadata.props,
      propsSummary: metadata.propsSummary,
      dirty: fiber.dirty,
      lastRebuildFrame: metadata.lastRebuildFrame,
      deps: metadata.deps.map((deps) => [...deps]),
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
    cache: null,
    cachedHitTargets: null,
    lastDeps: undefined,
    cacheAtlasEpoch: 0,
    lastProps: undefined,
    hooks: [],
    providedContext: null,
    providedValue: undefined,
    dirty: true,
    failed: false,
    surfaceHits: 0,
    surface: null,
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

export function withContextFrames<T>(frames: readonly ContextFrameSnapshot[], fn: () => T): T {
  const prev = g_context_frame_probe;
  g_context_frame_probe = frames;
  try {
    return fn();
  } finally {
    g_context_frame_probe = prev;
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

export function useEffect(fn: () => void | Cleanup, deps?: ReadonlyArray<unknown>): void {
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

// ---------------------------------------------------- render-target wiring

let g_render_target: Window | null = null;

export function setRenderTarget(win: Window | null): void {
  g_render_target = win;
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
  return true;
}

function reconcileChildren(parent: Fiber, nodes: readonly Node[]): Fiber[] {
  const liveKeys = new Set<string>();
  const ordered: Fiber[] = [];
  const order: string[] = [];
  for (let i = 0; i < nodes.length; ++i) {
    const k = childKey(nodes[i], i);
    if (isDevMode() && liveKeys.has(k)) throw duplicateKeyError(k, parent);
    liveKeys.add(k);
    let f = parent.children.get(k);
    if (f && !sameNodeIdentity(f.node, nodes[i])) {
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
  }
  // Drop fibers that disappeared this frame; run their cleanups.
  for (const [k, f] of parent.children) {
    if (!liveKeys.has(k)) {
      unmountFiber(f);
      parent.children.delete(k);
    }
  }
  parent.childOrder = order;
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

function requestRenderTargetRedraw(): void {
  const win = g_render_target;
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
): void {
  const children = reconcileChildren(owner, nodes);
  for (let i = 0; i < nodes.length; ++i) {
    renderNode(nodes[i], children[i], target, ctx);
  }
}

function renderBoundaryFallback(
  fiber: Fiber,
  child: BoundaryChild,
  target: CommandBuffer | Renderer,
  ctx: RenderCtx,
): void {
  const fallback = singleBoundaryNode(child);
  renderNodeList(fiber, [fallback], target, ctx);
}

function renderNode(
  node: Node,
  fiber: Fiber,
  target: CommandBuffer | Renderer,
  ctx: RenderCtx,
): void {
  fiber.node = node;
  updateFiberDebugNode(fiber, node);
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
    const epoch = atlasEpoch();
    if (
      memoInfo &&
      !fiber.dirty &&
      fiber.cache !== null &&
      fiber.lastProps !== undefined &&
      fiber.cacheAtlasEpoch === epoch &&
      memoInfo.areEqual(fiber.lastProps, node.props)
    ) {
      recordFiberCacheStatus(fiber, 'hit', false);
      queueInto(target, fiber.cache, undefined, undefined);
      RenderStats.recordCacheHit();
      return;
    }

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
    produced = propagateInternalComponentProps(node, produced);
    if (!produced) {
      fiber.cache = memoInfo ? new CommandBuffer() : fiber.cache;
      fiber.lastProps = memoInfo ? node.props : fiber.lastProps;
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
      return;
    }
    // Wrap the produced node as the single child of this fiber.
    if (memoInfo) {
      recordFiberCacheStatus(fiber, 'miss', true);
      RenderStats.recordCacheMiss();
      RenderStats.recordRebuild();
      const fresh = new CommandBuffer();
      renderNodeList(fiber, [produced], fresh, ctx);
      fiber.cache = fresh;
      fiber.cacheAtlasEpoch = epoch;
      fiber.lastProps = node.props;
      fiber.dirty = false;
      queueInto(target, fresh, undefined, undefined);
    } else {
      renderNodeList(fiber, [produced], target, ctx);
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
    const valueChanged =
      fiber.providedContext !== providerCtx ||
      !shallowEqualProps(fiber.providedValue, node.props.value);
    fiber.providedContext = providerCtx;
    fiber.providedValue = node.props.value;
    if (valueChanged) {
      for (const consumer of providerCtx._consumers) markDirty(consumer);
    }
    ctx.contextStack.push({ ctx: providerCtx, value: node.props.value });
    try {
      renderNodeList(fiber, normalizeBoundaryChildren(node.props.children), target, ctx);
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
    } finally {
      ctx.contextStack.pop();
    }
    return;
  }
  if (node.type === 'portal') {
    renderNodeList(fiber, normalizeBoundaryChildren(node.props.children), node.props.to, ctx);
    recordFiberCacheStatus(fiber, 'miss', true);
    fiber.dirty = false;
    return;
  }
  if (node.type === 'error-boundary') {
    const fresh = new CommandBuffer();
    const children = normalizeBoundaryChildren(node.props.children);
    try {
      renderNodeList(fiber, children, fresh, ctx);
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
      renderBoundaryFallback(fiber, fallback, target, ctx);
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
    }
    return;
  }
  if (node.type === 'suspense') {
    const fresh = new CommandBuffer();
    const children = normalizeBoundaryChildren(node.props.children);
    try {
      renderNodeList(fiber, children, fresh, ctx);
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
      queueInto(target, fresh, undefined, undefined);
    } catch (e) {
      if (!asThenable(e)) throw e;
      watchSuspenseWakeable(e);
      renderBoundaryFallback(fiber, node.props.fallback, target, ctx);
      fiber.dirty = false;
      recordFiberCacheStatus(fiber, 'miss', true);
    }
    return;
  }
  // Layer.
  const props = node.props;
  const wantDeps = props.deps;
  const layerEpoch = atlasEpoch();
  const cacheable =
    wantDeps !== undefined &&
    !fiber.dirty &&
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
      fiber.surface.bakedEpoch === cached.epoch() &&
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
      cached.vertexCount() >= SURFACE_MIN_VERTS
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
  const hitStart = hitTargetCount();
  const fresh = new CommandBuffer();
  renderNodeList(fiber, props.children, fresh, ctx);
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
  if (src.isEmpty()) return;
  if (transform && tint) target.queue(src, transform, tint);
  else if (transform) target.queue(src, transform);
  else target.queue(src);
}

function queuePaintFlashOutline(
  target: CommandBuffer | Renderer,
  src: CommandBuffer,
  transform: Mat4 | undefined,
): void {
  if (!isPaintFlashEnabled() || src.isEmpty()) return;
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

export interface RenderOptions {
  animate?: boolean;
  frameLoop?: FrameLoopOptions;
}

export function render(
  root: Node,
  target: CommandBuffer | Renderer,
  options?: RenderOptions,
): void {
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

const g_frame_callbacks: Array<(dtMs: number) => void> = [];

let g_render_frame_loop_dispose: FrameLoopDisposer | null = null;

function ensureRenderFrameLoop(options?: FrameLoopOptions): FrameLoopDisposer {
  if (!g_render_frame_loop_dispose) g_render_frame_loop_dispose = startFrameLoop(options);
  return g_render_frame_loop_dispose;
}

(globalThis as FxeUiFrameLoopBridgeGlobal).__fxeUiEnsureFrameLoop = () => ensureRenderFrameLoop();

export function startFrameLoop(options: FrameLoopOptions = {}): FrameLoopDisposer {
  const globals = globalThis as {
    requestAnimationFrame?: (fn: (timeMs: number) => void) => unknown;
    cancelAnimationFrame?: (id: unknown) => void;
  };
  const requestFrame = options.requestAnimationFrame ?? globals.requestAnimationFrame;
  if (typeof requestFrame !== 'function') {
    throw new Error('startFrameLoop() requires requestAnimationFrame');
  }
  const cancelFrame = options.cancelAnimationFrame ?? globals.cancelAnimationFrame;

  let disposed = false;
  let handle: unknown;
  let previousTimeMs: number | null = null;
  const step = (timeMs: number): void => {
    if (disposed) return;
    const dtMs = previousTimeMs === null ? 0 : Math.max(0, timeMs - previousTimeMs);
    previousTimeMs = timeMs;
    tickFrame(dtMs);
    if (!disposed) handle = requestFrame(step);
  };

  handle = requestFrame(step);
  const dispose = (): void => {
    if (disposed) return;
    disposed = true;
    if (typeof cancelFrame === 'function') cancelFrame(handle);
    if (g_render_frame_loop_dispose === dispose) g_render_frame_loop_dispose = null;
  };
  return dispose;
}

export function tickFrame(dtMs: number): void {
  ++g_tick_frame_counter;
  tickSchedulerFrame(dtMs);
  tickAnimatedFrames(dtMs);
  for (const fn of g_frame_callbacks) {
    try {
      fn(dtMs);
    } catch (e) {
      console.error(`useFrame threw: ${e}`);
    }
  }
}
