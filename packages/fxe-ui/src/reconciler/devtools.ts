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

const globals = globalThis as FxeDevtoolsGlobal;
globals.__fxe_devtools = {
  ...(globals.__fxe_devtools ?? {}),
  fiberTree: snapshotFiberTree,
  setPaintFlash,
};
