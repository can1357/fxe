import { type CommandBuffer, OffscreenRenderer, Primitives, type Renderer } from 'fxe';

export const SURFACE_BAKE_HITS = 4;
export const SURFACE_MIN_VERTS = 200;
export const SURFACE_SLOT_CAP = 4;
export const SURFACE_PAD = 1;

export const SURFACE_CACHE_DISABLED = process.env.FXE_DISABLE_SURFACE_CACHE === '1';

export function atlasEpoch(): number {
  return Primitives.atlasEpoch();
}

export interface SurfaceCacheEntry {
  off: OffscreenRenderer;
  slot: number;
  x: number;
  y: number;
  width: number;
  height: number;
  renderer: Renderer;
  bakedEpoch: number;
  atlasEpoch: number;
}

export interface RendererSurfaceState {
  free: number[];
  bound: (OffscreenRenderer | null)[];
}

export const kRendererSurfaceState = Symbol('fxe-ui.rendererSurfaceState');

export function rendererSurfaceState(r: Renderer): RendererSurfaceState {
  let s = Reflect.get(r, kRendererSurfaceState) as RendererSurfaceState | undefined;
  if (!s) {
    s = {
      free: [0, 1, 2, 3].slice(0, SURFACE_SLOT_CAP),
      bound: new Array(SURFACE_SLOT_CAP).fill(null),
    };
    Reflect.set(r, kRendererSurfaceState, s);
  }
  return s;
}

export function allocSurfaceSlot(r: Renderer): number | null {
  const s = rendererSurfaceState(r);
  return s.free.length > 0 ? (s.free.shift() as number) : null;
}

export function freeSurfaceSlot(r: Renderer, slot: number): void {
  const s = rendererSurfaceState(r);
  if (!s.free.includes(slot)) s.free.push(slot);
}

export function bindRendererUserTexture(
  r: Renderer,
  slot: number,
  source: OffscreenRenderer | null,
): void {
  r.bindUserTexture(slot, source);
  const s = rendererSurfaceState(r);
  if (slot >= 0 && slot < s.bound.length) s.bound[slot] = source;
}

export function translateMat(tx: number, ty: number): Float32Array {
  const m = new Float32Array(16);
  m[0] = 1;
  m[5] = 1;
  m[10] = 1;
  m[15] = 1;
  m[12] = tx;
  m[13] = ty;
  return m;
}

export function bakeFiberSurface(
  cache: CommandBuffer,
  renderer: Renderer,
): SurfaceCacheEntry | null {
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
  const parentState = rendererSurfaceState(renderer);
  for (let i = 0; i < parentState.bound.length; i++) {
    const src = parentState.bound[i];
    if (src !== null && src !== off) off.bindUserTexture(i, src);
  }
  off.queue(cache, translateMat(-bb.x + pad, -bb.y + pad));
  off.endFrame();
  bindRendererUserTexture(renderer, slot, off);
  return {
    off,
    slot,
    x: bb.x - pad,
    y: bb.y - pad,
    width: w,
    height: h,
    renderer,
    bakedEpoch: cache.__fxe_epoch,
    atlasEpoch: atlasEpoch(),
  };
}

export function releaseSurface(entry: SurfaceCacheEntry): void {
  try {
    bindRendererUserTexture(entry.renderer, entry.slot, null);
  } catch (_e) {
    /* renderer may already be torn down */
  }
  freeSurfaceSlot(entry.renderer, entry.slot);
}
