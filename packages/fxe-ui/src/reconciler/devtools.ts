export type DevtoolsFiberCacheHit = boolean | null;

export interface DevtoolsFiberNode {
  id: number;
  type: string;
  displayName: string | null;
  key: string;
  props: string;
  dirty: boolean;
  lastRebuildFrame: number;
  cacheHit: DevtoolsFiberCacheHit;
  deps: unknown[][];
  children: DevtoolsFiberNode[];
}

export interface DevtoolsFiberTreeSnapshot {
  tree: DevtoolsFiberNode[];
}

type SnapshotProvider = () => DevtoolsFiberTreeSnapshot;
type FxeDevtoolsGlobal = typeof globalThis & {
  __fxe_devtools?: {
    fiberTree: () => DevtoolsFiberTreeSnapshot;
    setPaintFlash: (enabled: boolean) => void;
    setMemoTrace: (enabled: boolean) => void;
    memoTraceSnapshot: () => MemoTraceSnapshot | null;
    resetMemoTrace: () => void;
  };
};

let paintFlash = false;
let snapshotProvider: SnapshotProvider = () => ({ tree: [] });

export function installFiberTreeSnapshotProvider(provider: SnapshotProvider): void {
  snapshotProvider = provider;
}

export function snapshotFiberTree(): DevtoolsFiberTreeSnapshot {
  return snapshotProvider();
}

export function setPaintFlash(enabled: boolean): void {
  paintFlash = enabled;
}

export function isPaintFlashEnabled(): boolean {
  return paintFlash;
}

// ----------------------------------------------------------------- memo trace
//
// Opt-in diagnostic for memo() bail decisions. When enabled, every memoised
// component records why it took the rebuild path (dirty / layout / noCache /
// noLastProps / epoch / propsDiff) or the bail path (hit), aggregated globally
// and per displayName. The first propsDiff per name also captures a shallow
// dump of last vs next props so callers can see exactly which key changed.
//
// Cost when disabled: one nullable load + branch per memoised component per
// render. No allocation, no globalThis pollution, no JSON cost.

export interface MemoTraceSlot {
  total: number;
  dirty: number;
  layout: number;
  noCache: number;
  noLastProps: number;
  epoch: number;
  propsDiff: number;
  hit: number;
}

export interface MemoTracePropsDump {
  last: unknown;
  next: unknown;
  lastKeys: string[];
  nextKeys: string[];
}

interface MemoTraceState {
  totals: MemoTraceSlot;
  byName: Map<string, MemoTraceSlot>;
  propsDump: Map<string, MemoTracePropsDump>;
}

export interface MemoTraceSnapshot {
  totals: MemoTraceSlot;
  byName: Record<string, MemoTraceSlot>;
  propsDump: Record<string, MemoTracePropsDump>;
}

function emptyMemoSlot(): MemoTraceSlot {
  return {
    total: 0,
    dirty: 0,
    layout: 0,
    noCache: 0,
    noLastProps: 0,
    epoch: 0,
    propsDiff: 0,
    hit: 0,
  };
}

let memoTrace: MemoTraceState | null = null;

export function setMemoTrace(enabled: boolean): void {
  memoTrace = enabled ? { totals: emptyMemoSlot(), byName: new Map(), propsDump: new Map() } : null;
}

export function resetMemoTrace(): void {
  if (memoTrace) setMemoTrace(true);
}

export function memoTraceState(): MemoTraceState | null {
  return memoTrace;
}

export function memoTraceSlotFor(displayName: string): MemoTraceSlot | null {
  if (!memoTrace) return null;
  let slot = memoTrace.byName.get(displayName);
  if (!slot) {
    slot = emptyMemoSlot();
    memoTrace.byName.set(displayName, slot);
  }
  return slot;
}

export function memoTraceSnapshot(): MemoTraceSnapshot | null {
  if (!memoTrace) return null;
  const byName: Record<string, MemoTraceSlot> = {};
  for (const [k, v] of memoTrace.byName) byName[k] = { ...v };
  const propsDump: Record<string, MemoTracePropsDump> = {};
  for (const [k, v] of memoTrace.propsDump) propsDump[k] = v;
  return { totals: { ...memoTrace.totals }, byName, propsDump };
}

const globals = globalThis as FxeDevtoolsGlobal;
globals.__fxe_devtools = {
  ...(globals.__fxe_devtools ?? {}),
  fiberTree: snapshotFiberTree,
  setPaintFlash,
  setMemoTrace,
  memoTraceSnapshot,
  resetMemoTrace,
};
